#pragma once

// SsrPass — stochastic screen-space reflections (ROADMAP R21).
//
// Three compute passes at the render extent, declared after the Hi-Z
// pyramid and before lighting:
//   ssr_trace     one GGX visible-normal ray per pixel below the roughness
//                 cutoff, marched through the Hi-Z pyramid's nearest depth
//                 (hierarchical min-Z traversal), validated against the
//                 thickness, the background and back faces, faded at the
//                 screen edges;
//   ssr_resolve   ray reuse between neighbours weighted by this pixel's
//                 GGX lobe over each ray's pdf (Stachowiak 2015), shading
//                 hits with the previous frame's lit HDR at their
//                 reprojected position;
//   ssr_temporal  accumulation with last frame's result (reprojected by
//                 velocity, clamped to the current neighbourhood).
// The result (rgb radiance, a confidence) goes to the lighting pass
// (LightingTuning::reflections), which blends the environment's
// prefiltered radiance under it: misses, rough surfaces and the screen
// edges fall back to the IBL cube.
//
// add_color_copy() keeps the colour the next frame's rays read: this
// frame's final render-extent HDR (lighting + transparent + particles)
// copied into a persistent image. Both that image and the temporal history
// are owned here and shared by the frame slots; each frame imports them in
// the state the previous frame left them (cross-frame hazards are the
// graph's first barriers), as the TAA history does. Compatible with TAAU:
// everything runs before the upscaler, with the jittered matrices the depth
// buffer was rendered with and R11's velocity.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/reflection_settings.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

inline constexpr VkFormat ssr_format = VK_FORMAT_R16G16B16A16_SFLOAT;

struct SsrInputs {
    rhi::ResourceHandle depth;    // GBuffer depth (render extent)
    rhi::ResourceHandle normal;   // GBuffer octahedral world normal
    rhi::ResourceHandle material; // GBuffer metallic / roughness / occlusion / coverage
    rhi::ResourceHandle velocity; // R11 velocity (uv_current - uv_previous)
    HiZTargets          hiz;      // of the same depth buffer
    glm::mat4           view_proj{1.0F}; // jittered, as the GBuffer was rendered
    glm::vec3           camera_pos{0.0F};
    VkExtent2D          extent{0, 0};
    std::uint64_t       frame_index = 0;
    bool                reset = false; // camera cut: no history, no previous colour
};

class SsrPass {
public:
    [[nodiscard]] static std::expected<SsrPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::uint32_t frames_in_flight);

    SsrPass();
    ~SsrPass();
    SsrPass(SsrPass&&) noexcept;
    SsrPass& operator=(SsrPass&&) noexcept;
    SsrPass(const SsrPass&) = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    [[nodiscard]] bool valid() const { return impl_ != nullptr; }

    // Declares trace, resolve and temporal for frame slot `slot` (whose
    // previous frame has finished). Returns the reflections (ssr_format,
    // render extent), or an invalid handle when `settings` disable SSR or
    // the inputs cannot support it (no Hi-Z, no velocity).
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot, const SsrInputs& in,
                 const SsrSettings& settings);

    // Declares the copy of `hdr` (render extent, TRANSFER_SRC usage) into
    // the colour the next frame's reflections read. Call after the last
    // pass writing `hdr` before the upscaler, in frames that called
    // add_to_graph with SSR on; other frames forget the colour.
    void add_color_copy(rhi::RenderGraph& graph, rhi::ResourceHandle hdr);

    // Forget the history and the previous colour (scene load, teleport).
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::renderer
