#pragma once

// ParticlePass — compute-driven GPU particles (ROADMAP R10.3). Settings,
// emission scheduling and the CPU references are in particles.hpp.
//
// One global particle pool (dead list + two ping-ponged alive lists) is shared
// by every emitter; each emitter owns a contiguous range of it (its cap).
// Per frame, all as render-graph passes:
//   particle_reset    (compute)  — initialise newly allocated emitter ranges;
//   particle_emit     (compute)  — deterministic emission from the dead list;
//   particle_commit   (compute)  — apply the pops, size the simulate dispatch;
//   particle_simulate (compute)  — indirect: forces, depth-buffer collision
//                                  against the GBuffer depth + normals, aging,
//                                  survivors / dead appended with atomics;
//   particle_finalize (compute)  — draw-indexed-indirect arguments;
//   particle_sort     (compute)  — optional bitonic back-to-front sort;
//   particles_wboit   (graphics) — alpha emitters into WBOIT accumulation +
//                                  revealage targets;
//   particles_composite (graphics) — resolve WBOIT over the HDR;
//   particles_additive  (graphics) — additive emitters straight into the HDR.
// The simulation passes are skipped on a frame with dt == 0 (SimClock paused):
// particles freeze but still draw. Draws are camera-facing quads, soft near
// geometry, lit per vertex by the sun, ambient and the clustered lights.
//
// The pool, the lists, the counters and the indirect arguments persist across
// frames (imported with the previous frame's accesses as their availability,
// so the graph orders frame N+1's first touch after frame N on the queue).
// Per-frame-in-flight state (emitter records, frame constants, descriptors)
// lives in slots: the caller passes the slot index and must have waited for
// that slot's previous frame before add_to_graph.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/particles.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// One emitter of one frame. `settings` must be sanitized and outlive the call.
struct ParticleEmitterInput {
    const ParticleEmitterSettings* settings = nullptr;
    EmitterFrameInfo               info{}; // info.sort_base is assigned by the pass
    VkImageView texture = VK_NULL_HANDLE;  // flipbook atlas (SHADER_READ_ONLY); null = soft disc
};

struct ParticleFrame {
    std::span<const ParticleEmitterInput> emitters{};
    float     dt = 0.0F;          // simulation step; 0 = paused (draw only)
    glm::mat4 view{1.0F};
    glm::mat4 proj{1.0F};         // the frame's (jittered) projection
    glm::vec3 camera{0.0F};
    DirectionalLightParams light{};
    VkExtent2D extent{0, 0};
};

// Scene inputs. `depth` / `normal` (GBuffer) are needed by collision and soft
// particles; `hdr` invalid = simulate only (no draw passes).
struct ParticleTargets {
    rhi::ResourceHandle depth{};
    rhi::ResourceHandle normal{};
    rhi::ResourceHandle hdr{};
    ClusteredLights     lights{};
};

// Per-frame GPU frame constants (std140, 384 bytes; ParticleFrame in
// lib/particle_common.slang).
struct ParticleFrameGpu {
    glm::vec4  view_proj[4]{};
    glm::vec4  inv_view_proj[4]{};
    glm::vec4  view[4]{};
    glm::vec4  inv_proj[4]{};
    glm::vec4  camera{0.0F};        // xyz, w = dt
    glm::vec4  camera_right{0.0F};
    glm::vec4  camera_up{0.0F};
    glm::vec4  sun_direction{0.0F};
    glm::vec4  sun_color{0.0F};
    glm::vec4  ambient{0.0F};
    glm::vec4  screen{0.0F};
    glm::uvec4 misc{0};             // y = pool capacity, w = emitter count
};
static_assert(sizeof(ParticleFrameGpu) == 384, "must match particle_common.slang ParticleFrame");

// The frame's particle buffers in the graph (for tests and tools).
struct ParticleGraphOutput {
    rhi::ResourceHandle particles{}; // ParticleGpu[pool]
    rhi::ResourceHandle alive{};     // uint[2 * pool]
    // uint4 per emitter slot: dead count, alive count of list 0 / 1, live list
    rhi::ResourceHandle counters{};
    rhi::ResourceHandle draw_args{}; // 8 uints per emitter (VkDrawIndexedIndirectCommand)
    std::uint32_t       pool_capacity = 0;
};

class ParticlePass {
public:
    [[nodiscard]] static std::expected<ParticlePass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const rhi::TransferContext& transfer, const std::string& cooked_shader_dir,
           std::uint32_t frames_in_flight, std::uint32_t pool_capacity = particle_default_pool);

    ParticlePass();
    ~ParticlePass();
    ParticlePass(ParticlePass&&) noexcept;
    ParticlePass& operator=(ParticlePass&&) noexcept;
    ParticlePass(const ParticlePass&) = delete;
    ParticlePass& operator=(const ParticlePass&) = delete;

    // Declares this frame's simulation and draws (see the header comment).
    // Emitters must lie inside the pool and number at most
    // particle_max_emitters; draw order across emitters is their order here.
    [[nodiscard]] std::expected<ParticleGraphOutput, core::Error>
    add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot, const ParticleFrame& frame,
                 const ParticleTargets& targets);

    [[nodiscard]] std::uint32_t pool_capacity() const;

    struct State; // defined in particle_internal.hpp

private:
    std::unique_ptr<State> state_;
};

} // namespace engine::renderer
