#include "engine/renderer/particle_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "particle_internal.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"particle pass: " + std::move(what)});
}

constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkDescriptorType   kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType   kStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType   kImage = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr std::uint32_t      kGroup = 64;   // particle_sim.slang numthreads
constexpr std::uint32_t      kSortBlock = 512; // particle_sort.slang BLOCK

// particle_sim.slang's resources by name, in this order (binding slots come
// from its reflection).
enum SimSlot : std::uint8_t { s_frame, s_emitters, s_particles, s_dead, s_alive, s_counters,
                              s_dispatch, s_draw, s_depth, s_normal };
constexpr std::array<rhi::BindingRequest, 10> kSimResources{{
    {"g_frame", kUniform}, {"g_emitters", kStorage}, {"g_particles", kStorage},
    {"g_dead", kStorage}, {"g_alive", kStorage}, {"g_counters", kStorage},
    {"g_dispatch", kStorage}, {"g_draw", kStorage}, {"g_depth", kImage}, {"g_normal", kImage},
}};
enum SortSlot : std::uint8_t { o_frame, o_emitters, o_particles, o_alive, o_counters, o_keys };
constexpr std::array<rhi::BindingRequest, 6> kSortResources{{
    {"g_frame", kUniform}, {"g_emitters", kStorage}, {"g_particles", kStorage},
    {"g_alive", kStorage}, {"g_counters", kStorage}, {"g_keys", kStorage},
}};
constexpr std::array<rhi::BindingRequest, 12> kDrawResources{{
    {"g_frame", kUniform}, {"g_emitters", kStorage}, {"g_particles", kStorage},
    {"g_alive", kStorage}, {"g_counters", kStorage}, {"g_depth", kImage}, {"g_texture", kImage},
    {"g_sampler", VK_DESCRIPTOR_TYPE_SAMPLER}, {"g_cluster_params", kUniform},
    {"g_lights", kStorage}, {"g_cluster_grid", kStorage}, {"g_cluster_indices", kStorage},
}};
constexpr std::array<rhi::BindingRequest, 2> kCompositeResources{{
    {"g_accum", kImage}, {"g_reveal", kImage},
}};

constexpr VkDeviceSize kFrameBytes = sizeof(ParticleFrameGpu);
constexpr VkDeviceSize kEmittersBytes =
    static_cast<VkDeviceSize>(particle_max_emitters) * sizeof(EmitterGpu);
constexpr VkDeviceSize kCountersBytes =
    static_cast<VkDeviceSize>(particle_max_emitters) * particle_counter_stride * 4;
constexpr VkDeviceSize kDispatchBytes =
    static_cast<VkDeviceSize>(particle_max_emitters) * particle_dispatch_stride * 4;
constexpr VkDeviceSize kDrawBytes =
    static_cast<VkDeviceSize>(particle_max_emitters) * particle_draw_stride * 4;

// What the persistent buffers' previous frame did to them: the first access
// of a frame waits for it (same queue, so a barrier orders it after).
constexpr VkPipelineStageFlags2 kPriorStages =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr VkAccessFlags2 kPriorAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
device_buffer(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(size,
                                   usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VMA_MEMORY_USAGE_AUTO);
}

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
host_buffer(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

// Loads `name`.spv and builds its set layout from reflection.
[[nodiscard]] std::expected<const rhi::LoadedShader*, core::Error>
load_with_layout(const rhi::Device& device, ParticlePass::State& s, rhi::PipelineCache& cache,
                 const std::string& dir, const char* name,
                 std::span<const rhi::BindingRequest> resources, VkShaderStageFlags stages,
                 rhi::DescriptorSetLayout& layout, std::vector<rhi::DescriptorBinding>& slots) {
    auto shader = cache.load(dir + "/" + name + ".spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, resources, stages);
    if (!bindings) return fail(std::string(name) + ": " + bindings.error().message);
    slots = std::move(*bindings);
    auto created = rhi::DescriptorSetLayout::create(device, s.fns, slots);
    if (!created) return std::unexpected(created.error());
    layout = std::move(*created);
    return *shader;
}

[[nodiscard]] VkPipelineColorBlendAttachmentState blend_state(VkBlendFactor src, VkBlendFactor dst) {
    VkPipelineColorBlendAttachmentState b{};
    b.blendEnable = VK_TRUE;
    b.srcColorBlendFactor = src;
    b.dstColorBlendFactor = dst;
    b.colorBlendOp = VK_BLEND_OP_ADD;
    b.srcAlphaBlendFactor = src;
    b.dstAlphaBlendFactor = dst;
    b.alphaBlendOp = VK_BLEND_OP_ADD;
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return b;
}

// Soft disc: white, alpha (1 - r^2)^2 — the sprite used without a flipbook.
[[nodiscard]] std::vector<std::uint8_t> soft_disc(std::uint32_t size) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(size) * size * 4, 255);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(size) * 2.0F - 1.0F;
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(size) * 2.0F - 1.0F;
            const float q = std::max(1.0F - (u * u + v * v), 0.0F);
            px[(static_cast<std::size_t>(y) * size + x) * 4 + 3] =
                static_cast<std::uint8_t>(std::lround(q * q * 255.0F));
        }
    }
    return px;
}

[[nodiscard]] ParticleFrameGpu make_frame_gpu(const ParticleFrame& f, std::uint32_t pool) {
    ParticleFrameGpu g;
    const glm::mat4 vp = f.proj * f.view;
    const glm::mat4 inv_vp = glm::inverse(vp);
    const glm::mat4 inv_proj = glm::inverse(f.proj);
    for (int c = 0; c < 4; ++c) {
        g.view_proj[c] = vp[c];
        g.inv_view_proj[c] = inv_vp[c];
        g.view[c] = f.view[c];
        g.inv_proj[c] = inv_proj[c];
    }
    g.camera = glm::vec4(f.camera, f.dt);
    g.camera_right = glm::vec4(f.view[0][0], f.view[1][0], f.view[2][0], 0.0F);
    g.camera_up = glm::vec4(f.view[0][1], f.view[1][1], f.view[2][1], 0.0F);
    g.sun_direction = f.light.direction;
    g.sun_color = glm::vec4(glm::vec3(f.light.color) * f.light.color.w, 0.0F);
    g.ambient = f.light.ambient;
    const float w = static_cast<float>(std::max(f.extent.width, 1U));
    const float h = static_cast<float>(std::max(f.extent.height, 1U));
    g.screen = glm::vec4(w, h, 1.0F / w, 1.0F / h);
    g.misc = glm::uvec4(0, pool, 0, static_cast<std::uint32_t>(f.emitters.size()));
    return g;
}

} // namespace

void particle_dispatch(VkCommandBuffer cmd, const rhi::ManagedPipeline& pipeline,
                       const rhi::DescriptorBuffer& set, const ParticlePush& push,
                       std::uint32_t groups) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    set.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0);
    vkCmdPushConstants(cmd, pipeline.layout(), kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups, 1, 1);
}

ParticlePass::ParticlePass() = default;
ParticlePass::~ParticlePass() = default;
ParticlePass::ParticlePass(ParticlePass&&) noexcept = default;
ParticlePass& ParticlePass::operator=(ParticlePass&&) noexcept = default;

std::uint32_t ParticlePass::pool_capacity() const { return state_ ? state_->pool : 0; }

std::expected<ParticlePass, core::Error>
ParticlePass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                     const std::string& dir, std::uint32_t frames_in_flight,
                     std::uint32_t pool_capacity) {
    if (frames_in_flight == 0) return fail("frames_in_flight must be > 0");
    if (pool_capacity == 0 || pool_capacity > (1U << 24)) return fail("pool capacity out of range");
    if (!device.independent_blend()) {
        return fail("weighted blended OIT needs the independentBlend feature");
    }
    ParticlePass out;
    out.state_ = std::make_unique<State>();
    State& s = *out.state_;
    s.device = &device;
    s.allocator = &allocator;
    s.pool = pool_capacity;
    s.key_capacity = std::max(2 * pool_capacity, 8 * kSortBlock);
    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("descriptor buffer functions unavailable");

    // ---- Compute: simulation + sort ----------------------------------------
    const VkPushConstantRange compute_push{kCompute, 0, sizeof(ParticlePush)};
    const auto make_compute = [&](const rhi::LoadedShader* shader, const VkDescriptorSetLayout& set,
                                  const char* entry,
                                  rhi::ManagedPipeline& pipeline) -> std::expected<void, core::Error> {
        rhi::ComputePipeline::CreateInfo ci{};
        ci.shader = shader;
        ci.entry = entry;
        ci.set_layouts = {&set, 1};
        ci.push_constants = {&compute_push, 1};
        auto p = cache.create_compute_pipeline(ci);
        if (!p) return std::unexpected(p.error());
        pipeline = std::move(*p);
        return {};
    };
    auto sim = load_with_layout(device, s, cache, dir, "particle_sim", kSimResources, kCompute,
                                s.sim_layout, s.sim_slots);
    if (!sim) return std::unexpected(sim.error());
    const VkDescriptorSetLayout sim_set = s.sim_layout.handle();
    for (auto [entry, pipeline] :
         std::array<std::pair<const char*, rhi::ManagedPipeline*>, 5>{{
             {"reset_main", &s.reset}, {"emit_main", &s.emit}, {"commit_main", &s.commit},
             {"simulate_main", &s.simulate}, {"finalize_main", &s.finalize}}}) {
        if (auto r = make_compute(*sim, sim_set, entry, *pipeline); !r) return std::unexpected(r.error());
    }
    auto sort = load_with_layout(device, s, cache, dir, "particle_sort", kSortResources, kCompute,
                                 s.sort_layout, s.sort_slots);
    if (!sort) return std::unexpected(sort.error());
    const VkDescriptorSetLayout sort_set = s.sort_layout.handle();
    for (auto [entry, pipeline] :
         std::array<std::pair<const char*, rhi::ManagedPipeline*>, 5>{{
             {"keys_main", &s.sort_keys}, {"local_main", &s.sort_local},
             {"global_main", &s.sort_global}, {"merge_main", &s.sort_merge},
             {"scatter_main", &s.sort_scatter}}}) {
        if (auto r = make_compute(*sort, sort_set, entry, *pipeline); !r) return std::unexpected(r.error());
    }

    // ---- Graphics: quads (additive / WBOIT) + WBOIT composite ---------------
    constexpr VkShaderStageFlags draw_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    auto draw = load_with_layout(device, s, cache, dir, "particle_draw", kDrawResources,
                                 draw_stages, s.draw_layout, s.draw_slots);
    if (!draw) return std::unexpected(draw.error());
    const VkDescriptorSetLayout draw_set = s.draw_layout.handle();
    const VkPushConstantRange draw_push{draw_stages, 0, sizeof(ParticlePush)};
    rhi::GraphicsPipeline::CreateInfo gi{};
    gi.vertex = *draw;
    gi.fragment = *draw;
    gi.depth_format = gbuffer_depth_format;
    gi.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    gi.depth_write = false;
    gi.cull_mode = VK_CULL_MODE_NONE;
    gi.set_layouts = {&draw_set, 1};
    gi.push_constants = {&draw_push, 1};
    {
        const std::array<VkFormat, 1> formats{hdr_color_format};
        const std::array<VkPipelineColorBlendAttachmentState, 1> blends{
            blend_state(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE)};
        gi.color_formats = formats;
        gi.blend_attachments = blends;
        gi.fragment_entry = "fragment_additive";
        auto p = cache.create_graphics_pipeline(gi);
        if (!p) return std::unexpected(p.error());
        s.draw_additive = std::move(*p);
    }
    {
        const std::array<VkFormat, 2> formats{particle_accum_format, particle_reveal_format};
        const std::array<VkPipelineColorBlendAttachmentState, 2> blends{
            blend_state(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE),
            blend_state(VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR)};
        gi.color_formats = formats;
        gi.blend_attachments = blends;
        gi.fragment_entry = "fragment_wboit";
        auto p = cache.create_graphics_pipeline(gi);
        if (!p) return std::unexpected(p.error());
        s.draw_wboit = std::move(*p);
    }
    auto comp = load_with_layout(device, s, cache, dir, "particle_composite", kCompositeResources,
                                 VK_SHADER_STAGE_FRAGMENT_BIT, s.composite_layout,
                                 s.composite_slots);
    if (!comp) return std::unexpected(comp.error());
    {
        const VkDescriptorSetLayout comp_set = s.composite_layout.handle();
        const std::array<VkFormat, 1> formats{hdr_color_format};
        rhi::GraphicsPipeline::CreateInfo ci{};
        ci.vertex = *comp;
        ci.fragment = *comp;
        ci.color_formats = formats;
        ci.alpha_blend = true; // SRC_ALPHA, ONE_MINUS_SRC_ALPHA: alpha = 1 - revealage
        ci.set_layouts = {&comp_set, 1};
        auto p = cache.create_graphics_pipeline(ci);
        if (!p) return std::unexpected(p.error());
        s.composite = std::move(*p);
    }

    // ---- Persistent pool ------------------------------------------------------
    const auto pool_bytes = [&](VkDeviceSize per) { return per * pool_capacity; };
    struct Alloc {
        rhi::GpuBuffer*    buffer;
        VkDeviceSize       bytes;
        VkBufferUsageFlags usage;
    };
    for (const Alloc& a : std::array<Alloc, 7>{{
             {&s.particles, pool_bytes(sizeof(ParticleGpu)), 0},
             {&s.dead, pool_bytes(4), 0},
             {&s.alive, pool_bytes(8), 0},
             {&s.counters, kCountersBytes, 0},
             {&s.dispatch_args, kDispatchBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT},
             {&s.draw_args, kDrawBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT},
             {&s.keys, static_cast<VkDeviceSize>(s.key_capacity) * 8, 0}}}) {
        auto b = device_buffer(allocator, a.bytes, a.usage);
        if (!b) return std::unexpected(b.error());
        *a.buffer = std::move(*b);
    }
    constexpr std::array<std::uint32_t, 6> quad{0, 1, 2, 2, 1, 3};
    auto indices = rhi::upload_device_buffer(allocator, transfer, quad.data(), sizeof(quad),
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indices) return std::unexpected(indices.error());
    s.quad_indices = std::move(*indices);

    // ---- Built-in images ------------------------------------------------------
    constexpr std::uint32_t disc_size = 64;
    const std::vector<std::uint8_t> disc = soft_disc(disc_size);
    const VkExtent2D disc_extent{disc_size, disc_size};
    const std::uint32_t disc_mips = rhi::full_mip_levels(disc_extent);
    auto disc_image = rhi::upload_device_image(allocator, transfer, disc.data(), disc.size(),
                                               disc_extent, VK_FORMAT_R8G8B8A8_SRGB,
                                               VK_IMAGE_USAGE_SAMPLED_BIT, disc_mips);
    if (!disc_image) return std::unexpected(disc_image.error());
    s.disc = std::move(*disc_image);
    auto disc_view = rhi::create_image_view(device.handle(), s.disc.handle(),
                                            VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT,
                                            disc_mips);
    if (!disc_view) return std::unexpected(disc_view.error());
    s.disc_view = std::move(*disc_view);
    constexpr float far = 1.0F;
    auto far_image = rhi::upload_device_image(allocator, transfer, &far, sizeof(far), {1, 1},
                                              VK_FORMAT_R32_SFLOAT);
    if (!far_image) return std::unexpected(far_image.error());
    s.far_depth = std::move(*far_image);
    auto far_view = rhi::create_image_view(device.handle(), s.far_depth.handle(), VK_FORMAT_R32_SFLOAT);
    if (!far_view) return std::unexpected(far_view.error());
    s.far_depth_view = std::move(*far_view);
    constexpr std::array<std::uint16_t, 2> up{0, 0}; // oct(0, 0) = +Z; never read (no depth)
    auto normal_image = rhi::upload_device_image(allocator, transfer, up.data(), sizeof(up), {1, 1},
                                                 VK_FORMAT_R16G16_SFLOAT);
    if (!normal_image) return std::unexpected(normal_image.error());
    s.flat_normal = std::move(*normal_image);
    auto normal_view = rhi::create_image_view(device.handle(), s.flat_normal.handle(),
                                              VK_FORMAT_R16G16_SFLOAT);
    if (!normal_view) return std::unexpected(normal_view.error());
    s.flat_normal_view = std::move(*normal_view);
    // Clamped: flipbook cells must not bleed across the atlas edges.
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge, 16);
    if (!sampler) return std::unexpected(sampler.error());
    s.sampler = std::move(*sampler);
    auto empty = EmptyLightClusters::create(device, allocator);
    if (!empty) return std::unexpected(empty.error());
    s.empty_lights = std::move(*empty);

    // ---- Per-frame slots ------------------------------------------------------
    const VkDevice vk = device.handle();
    s.slots.resize(frames_in_flight);
    for (ParticleSlot& slot : s.slots) {
        auto frame = host_buffer(allocator, kFrameBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (!frame) return std::unexpected(frame.error());
        slot.frame = std::move(*frame);
        auto emitters = host_buffer(allocator, kEmittersBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!emitters) return std::unexpected(emitters.error());
        slot.emitters = std::move(*emitters);
        for (auto [set, layout] : std::array<std::pair<rhi::DescriptorBuffer*, rhi::DescriptorSetLayout*>, 3>{
                 {{&slot.sim_set, &s.sim_layout}, {&slot.sort_set, &s.sort_layout},
                  {&slot.composite_set, &s.composite_layout}}}) {
            auto db = rhi::DescriptorBuffer::create(device, allocator, s.fns, *layout);
            if (!db) return std::unexpected(db.error());
            *set = std::move(*db);
        }
        // The buffers never move: everything but the GBuffer images is written once.
        const VkDeviceAddress frame_addr = rhi::buffer_device_address(vk, slot.frame);
        const VkDeviceAddress emitters_addr = rhi::buffer_device_address(vk, slot.emitters);
        const auto& sim_b = s.sim_slots;
        slot.sim_set.write_uniform_buffer(sim_b[s_frame].binding, frame_addr, kFrameBytes);
        slot.sim_set.write_storage_buffer(sim_b[s_emitters].binding, emitters_addr, kEmittersBytes);
        const auto storage = [&](rhi::DescriptorBuffer& set, std::uint32_t binding,
                                 const rhi::GpuBuffer& buffer, VkDeviceSize bytes) {
            set.write_storage_buffer(binding, rhi::buffer_device_address(vk, buffer), bytes);
        };
        storage(slot.sim_set, sim_b[s_particles].binding, s.particles, pool_bytes(sizeof(ParticleGpu)));
        storage(slot.sim_set, sim_b[s_dead].binding, s.dead, pool_bytes(4));
        storage(slot.sim_set, sim_b[s_alive].binding, s.alive, pool_bytes(8));
        storage(slot.sim_set, sim_b[s_counters].binding, s.counters, kCountersBytes);
        storage(slot.sim_set, sim_b[s_dispatch].binding, s.dispatch_args, kDispatchBytes);
        storage(slot.sim_set, sim_b[s_draw].binding, s.draw_args, kDrawBytes);
        const auto& sort_b = s.sort_slots;
        slot.sort_set.write_uniform_buffer(sort_b[o_frame].binding, frame_addr, kFrameBytes);
        slot.sort_set.write_storage_buffer(sort_b[o_emitters].binding, emitters_addr, kEmittersBytes);
        storage(slot.sort_set, sort_b[o_particles].binding, s.particles, pool_bytes(sizeof(ParticleGpu)));
        storage(slot.sort_set, sort_b[o_alive].binding, s.alive, pool_bytes(8));
        storage(slot.sort_set, sort_b[o_counters].binding, s.counters, kCountersBytes);
        storage(slot.sort_set, sort_b[o_keys].binding, s.keys,
                static_cast<VkDeviceSize>(s.key_capacity) * 8);
    }
    return out;
}

std::expected<ParticleGraphOutput, core::Error>
ParticlePass::add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot_index,
                           const ParticleFrame& frame, const ParticleTargets& targets) {
    if (!state_) return fail("not created");
    State& s = *state_;
    if (slot_index >= s.slots.size()) return fail("frame slot out of range");
    if (frame.emitters.size() > particle_max_emitters) return fail("too many emitters");
    ParticleSlot& slot = s.slots[slot_index];
    const bool simulate = frame.dt > 0.0F && std::isfinite(frame.dt);

    // Emitter records (host writes land before vkQueueSubmit — no barrier).
    std::vector<std::uint32_t> resets;
    std::vector<std::uint32_t> sorted;
    std::uint32_t key_cursor = 0;
    std::uint32_t slot_mask[particle_max_emitters / 32]{};
    auto* records = static_cast<EmitterGpu*>(slot.emitters.mapped());
    for (std::size_t i = 0; i < frame.emitters.size(); ++i) {
        const ParticleEmitterInput& in = frame.emitters[i];
        if (in.settings == nullptr) return fail("emitter without settings");
        EmitterFrameInfo info = in.info;
        const std::uint32_t capacity = in.settings->max_particles;
        if (info.slot >= particle_max_emitters) return fail("emitter slot out of range");
        if ((slot_mask[info.slot / 32] >> (info.slot % 32) & 1U) != 0) {
            return fail("emitter slot used twice");
        }
        slot_mask[info.slot / 32] |= 1U << (info.slot % 32);
        if (capacity == 0 || info.pool_base > s.pool || capacity > s.pool - info.pool_base) {
            return fail("emitter range outside the pool");
        }
        if (!simulate) info.spawn_count = 0;
        info.dt = simulate ? frame.dt : 0.0F;
        const std::uint32_t keys = particle_sort_size(*in.settings);
        info.sort_base = key_cursor;
        ParticleEmitterSettings settings = *in.settings;
        if (keys > 0 && keys <= s.key_capacity - key_cursor) {
            key_cursor += keys;
            sorted.push_back(static_cast<std::uint32_t>(i));
        } else {
            settings.sort = false; // no room left for its keys this frame
        }
        records[i] = make_emitter_gpu(settings, info);
        if (info.reset) resets.push_back(static_cast<std::uint32_t>(i));
    }
    if (!frame.emitters.empty()) {
        if (auto f = slot.emitters.flush(0, frame.emitters.size() * sizeof(EmitterGpu)); !f) {
            return std::unexpected(f.error());
        }
    }
    const ParticleFrameGpu frame_gpu = make_frame_gpu(frame, s.pool);
    std::memcpy(slot.frame.mapped(), &frame_gpu, sizeof(frame_gpu));
    if (auto f = slot.frame.flush(0, kFrameBytes); !f) return std::unexpected(f.error());

    const auto import = [&](const char* name, const rhi::GpuBuffer& b, VkDeviceSize bytes) {
        return graph.import_buffer(name, b.handle(), bytes, kPriorStages, kPriorAccess);
    };
    ParticleGraphOutput out;
    out.pool_capacity = s.pool;
    out.particles = import("particles", s.particles, sizeof(ParticleGpu) * VkDeviceSize{s.pool});
    out.alive = import("particle_alive", s.alive, VkDeviceSize{8} * s.pool);
    out.counters = import("particle_counters", s.counters, kCountersBytes);
    out.draw_args = import("particle_draw_args", s.draw_args, kDrawBytes);
    const rhi::ResourceHandle dead = import("particle_dead", s.dead, VkDeviceSize{4} * s.pool);
    const rhi::ResourceHandle dispatch_args = import("particle_dispatch", s.dispatch_args, kDispatchBytes);
    if (frame.emitters.empty()) return out;

    const auto count = static_cast<std::uint32_t>(frame.emitters.size());
    // What the recorded dispatches need, by value (the inputs need not
    // outlive this call).
    struct Dispatch {
        std::uint32_t slot = 0;     // emitter slot: indirect argument offset
        std::uint32_t capacity = 0; // reset size
        std::uint32_t spawn = 0;    // emit size
    };
    std::vector<Dispatch> emitters(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        emitters[i] = {records[i].ids.z, records[i].range.y, records[i].range.z};
    }
    using U = rhi::ResourceUsage;
    if (!resets.empty()) {
        (void)graph.add_pass("particle_reset", rhi::PassType::compute)
            .writes(dead, U::buffer_write_compute)
            .writes(out.counters, U::buffer_write_compute)
            .writes(out.draw_args, U::buffer_write_compute)
            .writes(dispatch_args, U::buffer_write_compute)
            .execute([&s, set = &slot.sim_set, resets, emitters](rhi::PassContext& ctx) {
                for (const std::uint32_t i : resets) {
                    const std::uint32_t cap = emitters[i].capacity;
                    particle_dispatch(ctx.cmd(), s.reset, *set, {i, 0, 0, 0}, (cap + kGroup - 1) / kGroup);
                }
            });
    }
    if (simulate) {
        // GBuffer depth + normals for collision (stand-ins when not given).
        const bool has_depth = targets.depth.valid() && targets.normal.valid();
        (void)graph.add_pass("particle_emit", rhi::PassType::compute)
            .reads(out.counters, U::buffer_read_compute)
            .reads(dead, U::buffer_read_compute)
            .writes(out.particles, U::buffer_write_compute)
            .writes(out.alive, U::buffer_write_compute)
            .execute([&s, set = &slot.sim_set, emitters](rhi::PassContext& ctx) {
                for (std::uint32_t i = 0; i < emitters.size(); ++i) {
                    const std::uint32_t n = emitters[i].spawn;
                    if (n == 0) continue;
                    particle_dispatch(ctx.cmd(), s.emit, *set, {i, 0, 0, 0}, (n + kGroup - 1) / kGroup);
                }
            });
        (void)graph.add_pass("particle_commit", rhi::PassType::compute)
            .writes(out.counters, U::buffer_write_compute)
            .writes(dispatch_args, U::buffer_write_compute)
            .execute([&s, set = &slot.sim_set, count](rhi::PassContext& ctx) {
                particle_dispatch(ctx.cmd(), s.commit, *set, {count, 0, 0, 0}, (count + kGroup - 1) / kGroup);
            });
        auto simulate_pass = graph.add_pass("particle_simulate", rhi::PassType::compute);
        simulate_pass.reads(dispatch_args, U::buffer_read_indirect)
            .writes(out.particles, U::buffer_write_compute)
            .writes(out.alive, U::buffer_write_compute)
            .writes(dead, U::buffer_write_compute)
            .writes(out.counters, U::buffer_write_compute);
        if (has_depth) {
            simulate_pass.reads(targets.depth, U::sampled_compute)
                .reads(targets.normal, U::sampled_compute);
        }
        simulate_pass.execute([&s, set = &slot.sim_set, emitters, has_depth, targets,
                               dispatch_args](rhi::PassContext& ctx) {
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            set->write_sampled_image(s.sim_slots[s_depth].binding,
                                     has_depth ? ctx.resolve(targets.depth).view
                                               : s.far_depth_view.handle(), ro);
            set->write_sampled_image(s.sim_slots[s_normal].binding,
                                     has_depth ? ctx.resolve(targets.normal).view
                                               : s.flat_normal_view.handle(), ro);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.simulate.handle());
            set->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.simulate.layout(), 0);
            const VkBuffer args = ctx.resolve(dispatch_args).buffer;
            for (std::uint32_t i = 0; i < emitters.size(); ++i) {
                const ParticlePush push{i, 0, 0, 0};
                vkCmdPushConstants(cmd, s.simulate.layout(), kCompute, 0, sizeof(push), &push);
                const VkDeviceSize offset = VkDeviceSize{emitters[i].slot} *
                                            particle_dispatch_stride * 4;
                vkCmdDispatchIndirect(cmd, args, offset);
            }
        });
        (void)graph.add_pass("particle_finalize", rhi::PassType::compute)
            .writes(out.counters, U::buffer_write_compute)
            .writes(out.draw_args, U::buffer_write_compute)
            .execute([&s, set = &slot.sim_set, count](rhi::PassContext& ctx) {
                particle_dispatch(ctx.cmd(), s.finalize, *set, {count, 0, 0, 0}, (count + kGroup - 1) / kGroup);
            });
    }
    if (!sorted.empty()) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> jobs; // emitter, key count
        for (const std::uint32_t i : sorted) jobs.emplace_back(i, records[i].ids.y);
        declare_particle_sort(s, slot, graph, out,
                              import("particle_sort_keys", s.keys, VkDeviceSize{8} * s.key_capacity),
                              std::move(jobs));
    }

    if (!targets.hdr.valid()) return out;
    if (!targets.depth.valid()) return fail("drawing needs the scene depth");
    ParticleDrawContext context;
    context.particles = out.particles;
    context.alive = out.alive;
    context.counters = out.counters;
    context.draw_args = out.draw_args;
    context.frame_address = rhi::buffer_device_address(s.device->handle(), slot.frame);
    context.emitters_address = rhi::buffer_device_address(s.device->handle(), slot.emitters);
    context.emitter_count = count;
    if (auto r = declare_particle_draws(s, slot, graph, frame, targets, context); !r) {
        return std::unexpected(r.error());
    }
    return out;
}

} // namespace engine::renderer
