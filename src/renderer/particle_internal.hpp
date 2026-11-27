#pragma once

// Private to particle_pass.cpp (creation, simulation, sort) and
// particle_draw.cpp (draw passes): the pass state and the draw entry point.

#include <cstdint>
#include <utility>
#include <vector>

#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/particle_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::renderer {

// Push constants shared by every particle shader (16 bytes).
struct ParticlePush {
    std::uint32_t emitter = 0; // emitter index (or count, for per-emitter passes)
    std::uint32_t k = 0;       // bitonic stage
    std::uint32_t j = 0;       // bitonic distance
    std::uint32_t pad = 0;
};

inline constexpr std::uint32_t particle_counter_stride = 4;  // uints per emitter
inline constexpr std::uint32_t particle_dispatch_stride = 4; // uints per emitter
inline constexpr std::uint32_t particle_draw_stride = 8;     // uints per emitter
inline constexpr VkFormat      particle_accum_format = VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat      particle_reveal_format = VK_FORMAT_R16_SFLOAT;

struct ParticleSlot {
    rhi::GpuBuffer                     frame;    // host-visible ParticleFrameGpu
    rhi::GpuBuffer                     emitters; // host-visible EmitterGpu[particle_max_emitters]
    rhi::DescriptorBuffer              sim_set;
    rhi::DescriptorBuffer              sort_set;
    rhi::DescriptorBuffer              composite_set;
    std::vector<rhi::DescriptorBuffer> draw_sets; // one per drawn emitter (its texture)
};

// Heap-held so descriptor buffers' pointers to the layouts stay valid when
// the pass is moved.
struct ParticlePass::State {
    const rhi::Device*             device = nullptr;
    rhi::GpuAllocator*             allocator = nullptr;
    rhi::DescriptorBufferFunctions fns{};

    // Layouts with the reflected binding of each resource, in request order.
    rhi::DescriptorSetLayout             sim_layout;
    rhi::DescriptorSetLayout             sort_layout;
    rhi::DescriptorSetLayout             draw_layout;
    rhi::DescriptorSetLayout             composite_layout;
    std::vector<rhi::DescriptorBinding>  sim_slots;
    std::vector<rhi::DescriptorBinding>  sort_slots;
    std::vector<rhi::DescriptorBinding>  draw_slots;
    std::vector<rhi::DescriptorBinding>  composite_slots;

    rhi::ManagedPipeline reset;
    rhi::ManagedPipeline emit;
    rhi::ManagedPipeline commit;
    rhi::ManagedPipeline simulate;
    rhi::ManagedPipeline finalize;
    rhi::ManagedPipeline sort_keys;
    rhi::ManagedPipeline sort_local;
    rhi::ManagedPipeline sort_global;
    rhi::ManagedPipeline sort_merge;
    rhi::ManagedPipeline sort_scatter;
    rhi::ManagedPipeline draw_additive;
    rhi::ManagedPipeline draw_wboit;
    rhi::ManagedPipeline composite;

    // Persistent pool (device local).
    std::uint32_t  pool = 0;
    std::uint32_t  key_capacity = 0;
    rhi::GpuBuffer particles;
    rhi::GpuBuffer dead;
    rhi::GpuBuffer alive; // two lists of `pool` entries
    rhi::GpuBuffer counters;
    rhi::GpuBuffer dispatch_args;
    rhi::GpuBuffer draw_args;
    rhi::GpuBuffer keys;

    rhi::GpuBuffer     quad_indices; // 0 1 2 2 1 3
    rhi::GpuImage      disc;         // built-in soft disc (sRGB, mipped)
    rhi::ImageView     disc_view;
    rhi::GpuImage      far_depth;    // 1x1 R32F 1.0: stand-in when no depth is given
    rhi::ImageView     far_depth_view;
    rhi::GpuImage      flat_normal;  // 1x1 RG16F
    rhi::ImageView     flat_normal_view;
    rhi::Sampler       sampler;
    EmptyLightClusters empty_lights;

    std::uint32_t             cur_list = 0; // alive list the next simulation reads
    std::vector<ParticleSlot> slots;
};

// Binds `pipeline` + `set`, pushes `push` and dispatches `groups` x 1 x 1.
void particle_dispatch(VkCommandBuffer cmd, const rhi::ManagedPipeline& pipeline,
                       const rhi::DescriptorBuffer& set, const ParticlePush& push,
                       std::uint32_t groups);

// Declares particle_sort: a bitonic back-to-front sort of each job's drawn
// alive list (job = emitter index, power-of-two key count >= 512).
void declare_particle_sort(ParticlePass::State& state, ParticleSlot& slot, rhi::RenderGraph& graph,
                           const ParticleGraphOutput& buffers, rhi::ResourceHandle keys,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> jobs);

// What the draw passes need from add_to_graph.
struct ParticleDrawContext {
    rhi::ResourceHandle particles{};
    rhi::ResourceHandle alive{};
    rhi::ResourceHandle draw_args{};
    VkDeviceAddress     frame_address = 0;
    VkDeviceAddress     emitters_address = 0;
    std::uint32_t       emitter_count = 0;
};

// Declares particles_wboit + particles_composite (alpha emitters) and
// particles_additive into `targets.hdr`, depth-tested read-only against
// `targets.depth`, in `frame.emitters` order.
[[nodiscard]] std::expected<void, core::Error>
declare_particle_draws(ParticlePass::State& state, ParticleSlot& slot, rhi::RenderGraph& graph,
                       const ParticleFrame& frame, const ParticleTargets& targets,
                       const ParticleDrawContext& context);

} // namespace engine::renderer
