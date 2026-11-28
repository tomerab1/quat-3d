#pragma once

// DofPass — physically based, gather-based bokeh depth of field (ROADMAP R20).
//
// After Jimenez 2014 ("Next Generation Post Processing in Call of Duty:
// Advanced Warfare") and UE's DiaphragmDOF, in five compute dispatches:
//   1. dof_prepare   half-resolution colour + signed CoC (bilateral 2x2)
//   2. dof_tiles     per-tile (16 x 16 display px) largest near / far CoC
//   3. dof_dilate    tiles take the CoC of neighbours that reach them, so a
//                    blurred foreground bleeds over what is behind it
//   4. dof_gather    ring-pattern scatter-as-gather, split into a far field
//                    (normalised) and a near field with its own opacity
//   5. dof_composite full-resolution sharp image, far and near layers
//
// The circle of confusion is the thin-lens CoC of the active camera
// (physical_camera.hpp coc_params), evaluated per pixel from linear depth.
//
// Order: DoF runs after TAAU on the resolved display-extent colour (reading
// the render-extent depth through the display -> render scale), before motion
// blur — as HDRP orders them: blurred bokeh then moves rather than motion
// blur being defocused, and the temporal history never holds defocus.

#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/physical_camera.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

struct DofInputs {
    rhi::ResourceHandle color; // display extent, linear HDR (sampled)
    rhi::ResourceHandle depth; // render extent, [0, 1] depth (sampled)
    VkExtent2D          display_extent{0, 0};
    VkExtent2D          render_extent{0, 0};
    CocParams           coc;   // signed radius in DISPLAY pixels
    float               max_radius_px = 24.0F;
    int                 rings = 4;
    float               proj22 = 0.0F; // projection[2][2] (linear depth)
    float               proj32 = 0.0F; // projection[3][2]
};

class DofPass {
public:
    static constexpr VkFormat k_output_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    [[nodiscard]] static std::expected<DofPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    DofPass();
    ~DofPass();
    DofPass(DofPass&&) noexcept;
    DofPass& operator=(DofPass&&) noexcept;
    DofPass(const DofPass&) = delete;
    DofPass& operator=(const DofPass&) = delete;

    [[nodiscard]] bool valid() const { return state_ != nullptr; }

    // Declares the five dispatches; returns the defocused display-extent
    // image. Must be compiled and executed before the next call.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const DofInputs& in);

private:
    struct State; // stages, descriptor pool, sampler (dof_pass.cpp)
    std::unique_ptr<State> state_;
};

} // namespace engine::renderer
