#pragma once

// MotionBlurPass — reconstruction-filter motion blur (ROADMAP R20).
//
// McGuire, Hennessy, Bukowski and Osman 2012, "A Reconstruction Filter for
// Plausible Motion Blur" (with the sampling refinements of Jimenez 2014), in
// three compute dispatches over R11's velocity target:
//   1. motion_blur_tiles     longest half blur vector per K x K tile
//   2. motion_blur_neighbor  the dominant vector of each 3 x 3 tile block
//   3. motion_blur           per-pixel gather along it with soft depth
//                            ordering (foreground blurs over background)
// The blur is the per-frame velocity times the shutter fraction (the
// physical camera's shutter angle / 360, physical_camera.hpp), so blur
// length follows the shutter, not the frame rate.
//
// Runs on the display-extent colour after depth of field (dof_pass.hpp has
// the order), reading the render-extent velocity and depth.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

struct MotionBlurInputs {
    rhi::ResourceHandle color;    // display extent, linear HDR (sampled)
    rhi::ResourceHandle velocity; // render extent, velocity_format (sampled)
    rhi::ResourceHandle depth;    // render extent, [0, 1] depth (sampled)
    VkExtent2D          display_extent{0, 0};
    VkExtent2D          render_extent{0, 0};
    float               shutter_fraction = 0.5F; // shutter angle / 360 (x artistic scale)
    float               max_radius_px = 32.0F;   // also the tile size
    int                 samples = 12;
    float               proj22 = 0.0F; // projection[2][2] (linear depth)
    float               proj32 = 0.0F; // projection[3][2]
    std::uint32_t       frame_index = 0; // jitter seed
};

class MotionBlurPass {
public:
    static constexpr VkFormat k_output_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    [[nodiscard]] static std::expected<MotionBlurPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    MotionBlurPass();
    ~MotionBlurPass();
    MotionBlurPass(MotionBlurPass&&) noexcept;
    MotionBlurPass& operator=(MotionBlurPass&&) noexcept;
    MotionBlurPass(const MotionBlurPass&) = delete;
    MotionBlurPass& operator=(const MotionBlurPass&) = delete;

    [[nodiscard]] bool valid() const { return state_ != nullptr; }

    // Declares the three dispatches; returns the blurred display-extent image.
    // Must be compiled and executed before the next call.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const MotionBlurInputs& in);

private:
    struct State; // stages, descriptor pool (motion_blur_pass.cpp)
    std::unique_ptr<State> state_;
};

} // namespace engine::renderer
