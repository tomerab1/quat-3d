#include "engine/renderer/grass_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <optional>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "grass_params.hpp"

namespace engine::renderer {

namespace {

using grass_detail::GrassParamsGpu;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Mirrors PushConstants in grass.slang.
struct GrassDrawPush {
    std::uint32_t segments = 0;
    // First blade of this LOD's region in the blade buffer. Indirect draws
    // always use firstInstance = 0 (drawIndirectFirstInstance is not
    // required), so the region base travels here instead.
    std::uint32_t base = 0;
    std::uint32_t pad[2]{};
};
static_assert(sizeof(GrassDrawPush) == 16, "must match grass.slang PushConstants");

constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkShaderStageFlags kDrawStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr VkDeviceSize kParamsBytes = sizeof(GrassParamsGpu);
constexpr VkDeviceSize kArgsBytes = sizeof(GrassArgsGpu);
constexpr VkDeviceSize kBladeBytes = sizeof(GrassBladeGpu);

// grass_generate.slang set 0: params, blades, args, sampler, four height
// tiles, the GBuffer albedo + depth (root tint).
[[nodiscard]] std::array<rhi::DescriptorBinding, 10> generate_bindings() {
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kCompute},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kCompute},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kCompute},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kCompute},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
    }};
}

// grass_ground.slang set 0: params, sampler, four height tiles, GBuffer
// depth, GBuffer albedo (storage, read-modify-write).
[[nodiscard]] std::array<rhi::DescriptorBinding, 8> ground_bindings() {
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kCompute},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kCompute},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kCompute},
    }};
}

// grass.slang set 0: params, blades, wind field texture + its sampler, last
// frame's wind field (velocity).
[[nodiscard]] std::array<rhi::DescriptorBinding, 5> draw_bindings() {
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kDrawStages},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT},
    }};
}

// Strip of `segments` levels (2 * segments + 1 vertices) as a triangle list.
void append_strip(std::vector<std::uint32_t>& out, std::uint32_t segments) {
    for (std::uint32_t k = 0; k + 1 < segments; ++k) {
        const std::uint32_t a = 2 * k;
        out.insert(out.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
    }
    const std::uint32_t a = 2 * (segments - 1);
    out.insert(out.end(), {a, a + 1, a + 2});
}

} // namespace

std::expected<GrassPass, core::Error>
GrassPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                  const std::string& cooked_shader_dir, std::uint32_t frames_in_flight) {
    if (frames_in_flight == 0) return fail("grass pass: frames_in_flight must be > 0");
    GrassPass out;
    out.state_ = std::make_unique<State>();
    State& s = *out.state_;
    s.device = &device;
    s.allocator = &allocator;

    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("grass pass: descriptor buffer functions unavailable");

    if (auto r = out.create_pipelines(cache, cooked_shader_dir); !r) {
        return std::unexpected(r.error());
    }

    std::vector<std::uint32_t> indices;
    append_strip(indices, grass_near_segments);
    append_strip(indices, grass_far_segments);
    auto index_buffer = rhi::upload_device_buffer(allocator, transfer, indices.data(),
                                                  indices.size() * sizeof(std::uint32_t),
                                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!index_buffer) return std::unexpected(index_buffer.error());
    s.indices = std::move(*index_buffer);

    constexpr float k_flat = 0.0F;
    auto flat = rhi::upload_device_image(allocator, transfer, &k_flat, sizeof(k_flat), {1, 1},
                                         VK_FORMAT_R32_SFLOAT);
    if (!flat) return std::unexpected(flat.error());
    s.flat_height = std::move(*flat);
    auto flat_view = rhi::create_image_view(device.handle(), s.flat_height.handle(),
                                            VK_FORMAT_R32_SFLOAT);
    if (!flat_view) return std::unexpected(flat_view.error());
    s.flat_height_view = std::move(*flat_view);
    const std::uint16_t k_calm[4] = {0, 0, 0, 0x3C00}; // rgba16f (0, 0, 0, gust 1)
    auto calm = rhi::upload_device_image(allocator, transfer, k_calm, sizeof(k_calm), {1, 1},
                                         wind_field_format);
    if (!calm) return std::unexpected(calm.error());
    s.calm_wind = std::move(*calm);
    auto calm_view = rhi::create_image_view(device.handle(), s.calm_wind.handle(),
                                            wind_field_format);
    if (!calm_view) return std::unexpected(calm_view.error());
    s.calm_wind_view = std::move(*calm_view);
    const std::uint8_t k_no_albedo[4] = {0, 0, 0, 0};
    auto no_albedo = rhi::upload_device_image(allocator, transfer, k_no_albedo,
                                              sizeof(k_no_albedo), {1, 1}, gbuffer_albedo_format);
    if (!no_albedo) return std::unexpected(no_albedo.error());
    s.no_albedo = std::move(*no_albedo);
    auto no_albedo_view = rhi::create_image_view(device.handle(), s.no_albedo.handle(),
                                                 gbuffer_albedo_format);
    if (!no_albedo_view) return std::unexpected(no_albedo_view.error());
    s.no_albedo_view = std::move(*no_albedo_view);
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    s.sampler = std::move(*sampler);

    s.slots.resize(frames_in_flight);
    for (Slot& slot : s.slots) {
        auto params = allocator.create_buffer(
            kParamsBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!params) return std::unexpected(params.error());
        slot.params = std::move(*params);
        auto args = allocator.create_buffer(
            kArgsBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO);
        if (!args) return std::unexpected(args.error());
        slot.args = std::move(*args);
        auto gen_db = rhi::DescriptorBuffer::create(device, allocator, s.fns, s.generate_layout);
        if (!gen_db) return std::unexpected(gen_db.error());
        slot.generate_set = std::move(*gen_db);
        auto ground_db = rhi::DescriptorBuffer::create(device, allocator, s.fns, s.ground_layout);
        if (!ground_db) return std::unexpected(ground_db.error());
        slot.ground_set = std::move(*ground_db);
        auto draw_db = rhi::DescriptorBuffer::create(device, allocator, s.fns, s.draw_layout);
        if (!draw_db) return std::unexpected(draw_db.error());
        slot.draw_set = std::move(*draw_db);
        if (auto r = out.reserve(slot, 1); !r) return std::unexpected(r.error());
    }
    return out;
}

std::expected<void, core::Error> GrassPass::create_pipelines(rhi::PipelineCache& cache,
                                                             const std::string& dir) {
    State& s = *state_;
    const rhi::Device& device = *s.device;
    const auto make_layout = [&](std::span<const rhi::DescriptorBinding> bindings,
                                 rhi::DescriptorSetLayout& out) -> std::expected<void, core::Error> {
        auto layout = rhi::DescriptorSetLayout::create(device, s.fns, bindings);
        if (!layout) return std::unexpected(layout.error());
        out = std::move(*layout);
        return {};
    };
    const auto gen_bindings = generate_bindings();
    if (auto r = make_layout(gen_bindings, s.generate_layout); !r) return r;
    const auto gnd_bindings = ground_bindings();
    if (auto r = make_layout(gnd_bindings, s.ground_layout); !r) return r;
    const auto drw_bindings = draw_bindings();
    if (auto r = make_layout(drw_bindings, s.draw_layout); !r) return r;

    const auto compute = [&](const std::string& file, const char* entry,
                             const rhi::DescriptorSetLayout& layout,
                             rhi::ManagedPipeline& out) -> std::expected<void, core::Error> {
        auto shader = cache.load(dir + "/" + file);
        if (!shader) return std::unexpected(shader.error());
        const VkDescriptorSetLayout set = layout.handle();
        rhi::ComputePipeline::CreateInfo ci{};
        ci.shader = *shader;
        if (entry != nullptr) ci.entry = entry;
        ci.set_layouts = {&set, 1};
        auto pipeline = cache.create_compute_pipeline(ci);
        if (!pipeline) return std::unexpected(pipeline.error());
        out = std::move(*pipeline);
        return {};
    };
    if (auto r = compute("grass_generate.spv", nullptr, s.generate_layout, s.generate); !r) return r;
    if (auto r = compute("grass_generate.spv", "finalize_main", s.generate_layout, s.finalize); !r) {
        return r;
    }
    // The ground cover writes the GBuffer albedo as a storage image. RGBA8
    // storage is mandatory in Vulkan, but a translation layer may still lack
    // it (Metal read-write textures need tier 2): check, and run without the
    // cover rather than fail.
    VkFormatProperties albedo_props{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), gbuffer_albedo_format,
                                        &albedo_props);
    s.ground_cover_supported =
        (albedo_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    if (!s.ground_cover_supported) {
        std::fprintf(stderr, "[warn] grass: the GBuffer albedo format has no storage support; "
                             "ground cover disabled\n");
    } else if (auto r = compute("grass_ground.spv", nullptr, s.ground_layout, s.ground); !r) {
        return r;
    }

    auto draw_shader = cache.load(dir + "/grass.spv");
    if (!draw_shader) return std::unexpected(draw_shader.error());
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GrassDrawPush)};
    const VkDescriptorSetLayout draw_set = s.draw_layout.handle();
    rhi::GraphicsPipeline::CreateInfo gi{};
    gi.vertex = *draw_shader;
    gi.fragment = *draw_shader;
    gi.color_formats = gbuffer_color_formats;
    gi.depth_format = gbuffer_depth_format;
    gi.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    gi.cull_mode = VK_CULL_MODE_NONE;                  // blades are two-sided
    gi.set_layouts = {&draw_set, 1};
    gi.push_constants = {&push, 1};
    auto draw = cache.create_graphics_pipeline(gi);
    if (!draw) return std::unexpected(draw.error());
    s.draw = std::move(*draw);
    return {};
}

std::expected<void, core::Error> GrassPass::reserve(Slot& slot, std::uint32_t blades) {
    blades = std::max(blades, 1U);
    // Grow on demand; shrink once the need falls well below the allocation.
    if (blades <= slot.blade_capacity && blades * 4ULL >= slot.blade_capacity) return {};
    slot.blades = {};
    auto buffer = state_->allocator->create_buffer(
        static_cast<VkDeviceSize>(blades) * kBladeBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO);
    if (!buffer) {
        slot.blade_capacity = 0;
        return std::unexpected(buffer.error());
    }
    slot.blades = std::move(*buffer);
    slot.blade_capacity = blades;
    return {};
}

float grass_pixels_per_metre(const glm::mat4& projection, std::uint32_t height) {
    // A perspective projection has w = -z (column 2, row 3); an orthographic one does not.
    if (std::abs(projection[2][3]) < 0.5F) return 0.0F;
    return 0.5F * static_cast<float>(height) * std::abs(projection[1][1]);
}

std::expected<GrassOutput, core::Error>
GrassPass::add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot_index,
                        const GrassFrame& frame, const GBufferTargets* gbuffer,
                        VkExtent2D extent) {
    if (!state_) return fail("grass pass: not created");
    State& s = *state_;
    if (slot_index >= s.slots.size()) return fail("grass pass: frame slot out of range");
    Slot& slot = s.slots[slot_index];

    const GrassSettings settings = sanitize(frame.settings);
    const std::span<const TerrainHeightTile> tiles =
        frame.terrain.first(std::min<std::size_t>(frame.terrain.size(), grass_max_height_tiles));
    for (const TerrainHeightTile& t : tiles) {
        if (t.view == VK_NULL_HANDLE || t.resolution < 2 || !(t.size_m > 0.0F)) {
            return fail("grass pass: invalid terrain height tile");
        }
    }
    const GrassGrid grid = grass_grid(settings, frame.camera);
    const GrassCapacity capacity = grass_capacity(settings);
    if (grid.cell_size <= 0.0F || (settings.terrain_only && tiles.empty())) return GrassOutput{};

    if (auto r = reserve(slot, capacity.near + capacity.far); !r) return std::unexpected(r.error());

    // Host writes land before vkQueueSubmit, which makes them visible to the
    // device — no graph barrier needed for the params.
    const ViewMotion motion =
        gbuffer != nullptr ? gbuffer->motion : ViewMotion::still(frame.draw_view_proj);
    const GrassParamsGpu params = grass_detail::make_params(frame, settings, grid, capacity, tiles,
                                                            motion, extent, gbuffer != nullptr);
    std::memcpy(slot.params.mapped(), &params, sizeof(params));
    if (auto f = slot.params.flush(0, kParamsBytes); !f) return std::unexpected(f.error());

    // Descriptors are rewritten every frame: terrain tiles stream and the blade
    // buffer may have been resized. The slot's previous frame has finished.
    const VkDevice vk = s.device->handle();
    const VkDeviceAddress params_addr = rhi::buffer_device_address(vk, slot.params);
    const VkDeviceAddress blades_addr = rhi::buffer_device_address(vk, slot.blades);
    const VkDeviceSize blades_bytes = static_cast<VkDeviceSize>(slot.blade_capacity) * kBladeBytes;
    constexpr VkImageLayout k_read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    slot.generate_set.write_uniform_buffer(0, params_addr, kParamsBytes);
    slot.generate_set.write_storage_buffer(1, blades_addr, blades_bytes);
    slot.generate_set.write_storage_buffer(2, rhi::buffer_device_address(vk, slot.args), kArgsBytes);
    slot.generate_set.write_sampler(3, s.sampler.handle());
    slot.ground_set.write_uniform_buffer(0, params_addr, kParamsBytes);
    slot.ground_set.write_sampler(1, s.sampler.handle());
    for (std::uint32_t k = 0; k < grass_max_height_tiles; ++k) {
        const VkImageView view = k < tiles.size() ? tiles[k].view : s.flat_height_view.handle();
        slot.generate_set.write_sampled_image(4 + k, view, k_read);
        slot.ground_set.write_sampled_image(2 + k, view, k_read);
    }
    if (gbuffer == nullptr) { // no root tint (look.w = 0): stand-ins
        slot.generate_set.write_sampled_image(8, s.no_albedo_view.handle(), k_read);
        slot.generate_set.write_sampled_image(9, s.flat_height_view.handle(), k_read);
    }
    slot.draw_set.write_uniform_buffer(0, params_addr, kParamsBytes);
    slot.draw_set.write_storage_buffer(1, blades_addr, blades_bytes);
    slot.draw_set.write_sampler(3, s.sampler.handle());
    if (!frame.wind.valid()) {
        slot.draw_set.write_sampled_image(2, s.calm_wind_view.handle(), k_read);
        slot.draw_set.write_sampled_image(4, s.calm_wind_view.handle(), k_read);
    }

    GrassOutput out;
    out.capacity = capacity;
    out.grid = grid;
    out.blades = graph.import_buffer("grass_blades", slot.blades.handle(), blades_bytes);
    out.args = graph.import_buffer("grass_args", slot.args.handle(), kArgsBytes);

    const rhi::ResourceHandle args = out.args;
    const rhi::ResourceHandle blades = out.blades;
    (void)graph.add_pass("grass_reset", rhi::PassType::transfer)
        .writes(args, rhi::ResourceUsage::transfer_dst)
        .execute([args](rhi::PassContext& ctx) {
            vkCmdFillBuffer(ctx.cmd(), ctx.resolve(args).buffer, 0, kArgsBytes, 0);
        });

    if (gbuffer != nullptr && s.ground_cover_supported &&
        (settings.ground_cover > 0.0F || settings.far_ground_cover > 0.0F)) {
        add_ground_cover(graph, slot, *gbuffer, extent);
    }

    const glm::uvec2 groups = grid.groups;
    auto generate = graph.add_pass("grass_generate", rhi::PassType::compute);
    if (gbuffer != nullptr) {
        generate.reads(gbuffer->albedo, rhi::ResourceUsage::sampled_compute)
            .reads(gbuffer->depth, rhi::ResourceUsage::sampled_compute);
    }
    generate.writes(args, rhi::ResourceUsage::buffer_write_compute)
        .writes(blades, rhi::ResourceUsage::buffer_write_compute)
        .execute([state = &s, set = &slot.generate_set, groups,
                  targets = gbuffer != nullptr ? std::optional<GBufferTargets>(*gbuffer)
                                               : std::nullopt](rhi::PassContext& ctx) {
            if (targets) { // the transient views exist once the graph compiled
                set->write_sampled_image(8, ctx.resolve(targets->albedo).view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                set->write_sampled_image(9, ctx.resolve(targets->depth).view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->generate.handle());
            set->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->generate.layout(), 0);
            vkCmdDispatch(cmd, groups.x, groups.y, 1);
        });

    (void)graph.add_pass("grass_finalize", rhi::PassType::compute)
        .writes(args, rhi::ResourceUsage::buffer_write_compute)
        .execute([state = &s, set = &slot.generate_set](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->finalize.handle());
            set->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->finalize.layout(), 0);
            vkCmdDispatch(cmd, 1, 1, 1);
        });

    if (gbuffer != nullptr) add_draw(graph, slot, *gbuffer, extent, frame, out);
    return out;
}

void GrassPass::add_ground_cover(rhi::RenderGraph& graph, Slot& slot, const GBufferTargets& t,
                                 VkExtent2D extent) {
    const glm::uvec2 groups((extent.width + 7) / 8, (extent.height + 7) / 8);
    (void)graph.add_pass("grass_ground", rhi::PassType::compute)
        .reads(t.depth, rhi::ResourceUsage::sampled_compute)
        .writes(t.albedo, rhi::ResourceUsage::storage_read_write)
        .execute([state = state_.get(), set = &slot.ground_set, t, groups](rhi::PassContext& ctx) {
            set->write_sampled_image(6, ctx.resolve(t.depth).view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            set->write_storage_image(7, ctx.resolve(t.albedo).view, VK_IMAGE_LAYOUT_GENERAL);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->ground.handle());
            set->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->ground.layout(), 0);
            vkCmdDispatch(cmd, groups.x, groups.y, 1);
        });
}

void GrassPass::add_draw(rhi::RenderGraph& graph, Slot& slot, const GBufferTargets& t,
                         VkExtent2D extent, const GrassFrame& frame, const GrassOutput& out) {
    State& s = *state_;
    const rhi::ResourceHandle args = out.args;
    const rhi::ResourceHandle blades = out.blades;
    const rhi::ResourceHandle wind = frame.wind.texture;
    // Without last frame's field the blades get no field motion (this
    // frame's field on both sides).
    const rhi::ResourceHandle wind_prev =
        frame.wind.prev_texture.valid() ? frame.wind.prev_texture : wind;
    auto draw = graph.add_pass("grass_draw", rhi::PassType::graphics);
    if (wind.valid()) draw.reads(wind, rhi::ResourceUsage::sampled_graphics);
    if (wind_prev.valid() && wind_prev.index != wind.index) {
        draw.reads(wind_prev, rhi::ResourceUsage::sampled_graphics);
    }
    draw.reads(args, rhi::ResourceUsage::buffer_read_indirect)
        .reads(blades, rhi::ResourceUsage::buffer_read_vertex)
        .writes(t.albedo, rhi::ResourceUsage::color_attachment)
        .writes(t.normal, rhi::ResourceUsage::color_attachment)
        .writes(t.material, rhi::ResourceUsage::color_attachment)
        .writes(t.clearcoat, rhi::ResourceUsage::color_attachment)
        .writes(t.emissive, rhi::ResourceUsage::color_attachment)
        .writes(t.velocity, rhi::ResourceUsage::color_attachment)
        .writes(t.depth, rhi::ResourceUsage::depth_attachment)
        .execute([state = &s, set = &slot.draw_set, t, args, extent, wind, wind_prev,
                  far_base = out.capacity.near](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            if (wind.valid()) { // the transient views exist once the graph compiled
                set->write_sampled_image(2, ctx.resolve(wind).view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                set->write_sampled_image(4, ctx.resolve(wind_prev).view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            const auto color_attachment = [](VkImageView view) {
                VkRenderingAttachmentInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ai.imageView = view;
                ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // over mesh + terrain
                ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                return ai;
            };
            const std::array<VkRenderingAttachmentInfo, gbuffer_color_formats.size()> colors{
                color_attachment(ctx.resolve(t.albedo).view),
                color_attachment(ctx.resolve(t.normal).view),
                color_attachment(ctx.resolve(t.material).view),
                color_attachment(ctx.resolve(t.clearcoat).view),
                color_attachment(ctx.resolve(t.emissive).view),
                color_attachment(ctx.resolve(t.velocity).view)};
            VkRenderingAttachmentInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = ctx.resolve(t.depth).view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = &depth;
            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state->draw.handle());
            set->bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state->draw.layout(), 0);
            vkCmdBindIndexBuffer(cmd, state->indices.handle(), 0, VK_INDEX_TYPE_UINT32);
            const VkBuffer args_buffer = ctx.resolve(args).buffer;
            constexpr VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
            GrassDrawPush push{};
            push.segments = grass_near_segments;
            push.base = 0;
            vkCmdPushConstants(cmd, state->draw.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDrawIndexedIndirect(cmd, args_buffer, offsetof(GrassArgsGpu, near), 1, stride);
            push.segments = grass_far_segments;
            push.base = far_base;
            vkCmdPushConstants(cmd, state->draw.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDrawIndexedIndirect(cmd, args_buffer, offsetof(GrassArgsGpu, far), 1, stride);
            vkCmdEndRendering(cmd);
        });
}

} // namespace engine::renderer
