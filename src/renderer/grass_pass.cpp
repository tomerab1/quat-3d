#include "engine/renderer/grass_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Mirrors GrassParams in lib/grass_common.slang (std140, 480 bytes).
struct GrassParamsGpu {
    glm::mat4  view_proj{1.0F};
    glm::vec4  planes[6]{};
    glm::vec4  camera{0.0F};
    glm::ivec4 grid{0};
    glm::vec4  cell{0.0F};
    glm::vec4  lod{0.0F};
    glm::vec4  blade{0.0F};
    glm::vec4  limits{0.0F};
    glm::vec4  wind{0.0F};
    glm::vec4  base_color{0.0F};
    glm::vec4  tip_color{0.0F};
    glm::uvec4 counts{0};
    glm::uvec4 draw{0};
    glm::vec4  tile_rect[grass_max_height_tiles]{};
    glm::vec4  tile_info[grass_max_height_tiles]{};
    glm::vec4  ground{0.0F};
};
static_assert(sizeof(GrassParamsGpu) == 480, "must match grass_common.slang GrassParams");

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
constexpr std::uint32_t kNearIndices = grass_index_count(grass_near_segments);
constexpr std::uint32_t kFarIndices = grass_index_count(grass_far_segments);

// grass_generate.slang set 0: params, blades, args, sampler, four height tiles.
[[nodiscard]] std::array<rhi::DescriptorBinding, 8> generate_bindings() {
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kCompute},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kCompute},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kCompute},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kCompute},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
        {0, 7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kCompute},
    }};
}

// grass.slang set 0: params, blades.
[[nodiscard]] std::array<rhi::DescriptorBinding, 2> draw_bindings() {
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kDrawStages},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
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

[[nodiscard]] GrassParamsGpu make_params(const GrassFrame& frame, const GrassSettings& s,
                                         const GrassGrid& grid, const GrassCapacity& capacity,
                                         std::span<const TerrainHeightTile> tiles) {
    GrassParamsGpu p;
    p.view_proj = frame.draw_view_proj;
    const auto planes = grass_frustum_planes(frame.cull_view_proj);
    std::copy(planes.begin(), planes.end(), p.planes);
    p.camera = glm::vec4(frame.camera, frame.time_s);
    p.grid = glm::ivec4(grid.origin, glm::ivec2(grid.cells));
    p.cell = glm::vec4(grid.cell_size, s.radius, s.lod_start * s.radius,
                       s.lod_start * s.radius);
    p.lod = glm::vec4(s.far_density, s.radius * (1.0F - grass_edge_band), s.height_range.y,
                      grass_max_width_scale);
    p.blade = glm::vec4(s.height_range, s.width_range);
    const float max_cos = s.slope_deg.y >= 90.0F ? -2.0F : std::cos(glm::radians(s.slope_deg.y));
    const float min_cos = s.slope_deg.x <= 0.0F ? 2.0F : std::cos(glm::radians(s.slope_deg.x));
    p.limits = glm::vec4(max_cos, min_cos, s.height_m);
    p.wind = glm::vec4(s.wind_direction, s.wind_strength, s.wind_frequency);
    p.base_color = glm::vec4(s.base_color, s.terrain_only ? 1.0F : 0.0F);
    p.tip_color = glm::vec4(s.tip_color, s.ground_y);
    p.counts = glm::uvec4(s.seed, static_cast<std::uint32_t>(tiles.size()), capacity.near,
                          capacity.far);
    p.draw = glm::uvec4(kNearIndices, kFarIndices, grass_near_segments, grass_far_segments);

    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (std::size_t k = 0; k < tiles.size(); ++k) {
        const TerrainHeightTile& t = tiles[k];
        p.tile_rect[k] = glm::vec4(t.origin_xz, t.size_m, t.base_y);
        p.tile_info[k] = glm::vec4(1.0F / static_cast<float>(t.resolution),
                                   t.size_m / static_cast<float>(t.resolution - 1), 0.0F, 0.0F);
        lo = std::min(lo, t.base_y + t.min_height);
        hi = std::max(hi, t.base_y + t.max_height);
    }
    if (!s.terrain_only) {
        lo = std::min(lo, s.ground_y);
        hi = std::max(hi, s.ground_y);
    }
    p.ground = glm::vec4(lo, hi, 0.0F, 0.0F);
    return p;
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

    const auto gen_bindings = generate_bindings();
    auto gen_layout = rhi::DescriptorSetLayout::create(device, s.fns, gen_bindings);
    if (!gen_layout) return std::unexpected(gen_layout.error());
    s.generate_layout = std::move(*gen_layout);
    const auto drw_bindings = draw_bindings();
    auto drw_layout = rhi::DescriptorSetLayout::create(device, s.fns, drw_bindings);
    if (!drw_layout) return std::unexpected(drw_layout.error());
    s.draw_layout = std::move(*drw_layout);

    auto gen_shader = cache.load(cooked_shader_dir + "/grass_generate.spv");
    if (!gen_shader) return std::unexpected(gen_shader.error());
    const VkDescriptorSetLayout gen_set = s.generate_layout.handle();
    rhi::ComputePipeline::CreateInfo ci{};
    ci.shader = *gen_shader;
    ci.set_layouts = {&gen_set, 1};
    auto generate = cache.create_compute_pipeline(ci);
    if (!generate) return std::unexpected(generate.error());
    s.generate = std::move(*generate);
    ci.entry = "finalize_main";
    auto finalize = cache.create_compute_pipeline(ci);
    if (!finalize) return std::unexpected(finalize.error());
    s.finalize = std::move(*finalize);

    auto draw_shader = cache.load(cooked_shader_dir + "/grass.spv");
    if (!draw_shader) return std::unexpected(draw_shader.error());
    const std::array<VkFormat, 4> color_formats{gbuffer_albedo_format, gbuffer_normal_format,
                                                gbuffer_material_format, gbuffer_clearcoat_format};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GrassDrawPush)};
    const VkDescriptorSetLayout draw_set = s.draw_layout.handle();
    rhi::GraphicsPipeline::CreateInfo gi{};
    gi.vertex = *draw_shader;
    gi.fragment = *draw_shader;
    gi.color_formats = color_formats;
    gi.depth_format = gbuffer_depth_format;
    gi.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    gi.cull_mode = VK_CULL_MODE_NONE;                  // blades are two-sided
    gi.set_layouts = {&draw_set, 1};
    gi.push_constants = {&push, 1};
    auto draw = cache.create_graphics_pipeline(gi);
    if (!draw) return std::unexpected(draw.error());
    s.draw = std::move(*draw);

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
        auto draw_db = rhi::DescriptorBuffer::create(device, allocator, s.fns, s.draw_layout);
        if (!draw_db) return std::unexpected(draw_db.error());
        slot.draw_set = std::move(*draw_db);
        if (auto r = out.reserve(slot, 1); !r) return std::unexpected(r.error());
    }
    return out;
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
    const GrassParamsGpu params = make_params(frame, settings, grid, capacity, tiles);
    std::memcpy(slot.params.mapped(), &params, sizeof(params));
    if (auto f = slot.params.flush(0, kParamsBytes); !f) return std::unexpected(f.error());

    // Descriptors are rewritten every frame: terrain tiles stream and the blade
    // buffer may have been resized. The slot's previous frame has finished.
    const VkDevice vk = s.device->handle();
    const VkDeviceAddress params_addr = rhi::buffer_device_address(vk, slot.params);
    const VkDeviceAddress blades_addr = rhi::buffer_device_address(vk, slot.blades);
    const VkDeviceSize blades_bytes = static_cast<VkDeviceSize>(slot.blade_capacity) * kBladeBytes;
    slot.generate_set.write_uniform_buffer(0, params_addr, kParamsBytes);
    slot.generate_set.write_storage_buffer(1, blades_addr, blades_bytes);
    slot.generate_set.write_storage_buffer(2, rhi::buffer_device_address(vk, slot.args), kArgsBytes);
    slot.generate_set.write_sampler(3, s.sampler.handle());
    for (std::uint32_t k = 0; k < grass_max_height_tiles; ++k) {
        const VkImageView view = k < tiles.size() ? tiles[k].view : s.flat_height_view.handle();
        slot.generate_set.write_sampled_image(4 + k, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    slot.draw_set.write_uniform_buffer(0, params_addr, kParamsBytes);
    slot.draw_set.write_storage_buffer(1, blades_addr, blades_bytes);

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

    const glm::uvec2 groups = grid.groups;
    (void)graph.add_pass("grass_generate", rhi::PassType::compute)
        .writes(args, rhi::ResourceUsage::buffer_write_compute)
        .writes(blades, rhi::ResourceUsage::buffer_write_compute)
        .execute([state = &s, set = &slot.generate_set, groups](rhi::PassContext& ctx) {
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

    if (gbuffer == nullptr) return out;

    const GBufferTargets t = *gbuffer;
    graph.add_pass("grass_draw", rhi::PassType::graphics)
        .reads(args, rhi::ResourceUsage::buffer_read_indirect)
        .reads(blades, rhi::ResourceUsage::buffer_read_vertex)
        .writes(t.albedo, rhi::ResourceUsage::color_attachment)
        .writes(t.normal, rhi::ResourceUsage::color_attachment)
        .writes(t.material, rhi::ResourceUsage::color_attachment)
        .writes(t.clearcoat, rhi::ResourceUsage::color_attachment)
        .writes(t.depth, rhi::ResourceUsage::depth_attachment)
        .execute([state = &s, set = &slot.draw_set, t, args, extent,
                  far_base = capacity.near](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const auto color_attachment = [](VkImageView view) {
                VkRenderingAttachmentInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ai.imageView = view;
                ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // over mesh + terrain
                ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                return ai;
            };
            const std::array<VkRenderingAttachmentInfo, 4> colors{
                color_attachment(ctx.resolve(t.albedo).view),
                color_attachment(ctx.resolve(t.normal).view),
                color_attachment(ctx.resolve(t.material).view),
                color_attachment(ctx.resolve(t.clearcoat).view)};
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
    return out;
}

} // namespace engine::renderer
