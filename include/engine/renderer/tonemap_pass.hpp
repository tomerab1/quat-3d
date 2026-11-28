#pragma once

// TonemapPass — the final image (Phase 3.7; R20 cinematic post).
//
// A fullscreen graphics pass that samples the linear HDR colour (after the
// upscaler, depth of field, motion blur and bloom) and writes the LDR colour
// target — the swapchain image in the live path, or a transient image in
// tests: chromatic aberration, exposure, vignette, scene-referred grading,
// the tonemap operator, sRGB encoding, the look LUT and film grain
// (tonemap.slang, lib/color.slang). The pipelines bake the output colour
// format, so a pass instance is tied to the format it was created for.

#include <array>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
struct TransferContext;
}

namespace engine::renderer {

class ColorLutTexture;

// Everything after exposure (R20). The defaults reproduce the original pass:
// ACES, no grading, no LUT, no lens or film effects.
struct TonemapLook {
    TonemapOperator op = TonemapOperator::aces;
    GradingSettings grading{};
    FilmSettings    film{};
    // The look LUT (null or grading.lut_contribution 0: none). Must stay
    // alive until the graph has executed.
    const ColorLutTexture* lut = nullptr;
    // > 0: the physical camera's exposure multiplier (exposure_from_ev100),
    // which replaces auto exposure; the compensation still applies.
    float physical_exposure = 0.0F;
    float fov_y  = 1.0471976F; // vertical field of view (natural vignette)
    float time_s = 0.0F;       // film grain clock
};

class TonemapPass {
public:
    [[nodiscard]] static std::expected<TonemapPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, VkFormat output_format);

    TonemapPass() = default;
    TonemapPass(TonemapPass&&) noexcept = default;
    TonemapPass& operator=(TonemapPass&&) noexcept = default;
    TonemapPass(const TonemapPass&) = delete;
    TonemapPass& operator=(const TonemapPass&) = delete;

    // Add the final-image pass of `hdr` into the colour target `output` (which
    // must already be a graph resource — imported swapchain or transient). The
    // descriptor and parameter buffers are owned by the pass and hold this
    // call's values until the next call: add it once per graph execution.
    // `exposure_buffer` is the device address of the auto-exposure pass's adapted-
    // luminance buffer. Exposure = clamp(key / adapted, min, max) * 2^EV when it is
    // set and `exposure.auto_exposure` is on, the physical camera's exposure *
    // 2^EV when `look.physical_exposure` > 0, else 2^EV (the default settings
    // with no buffer give a multiplier of exactly 1).
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, rhi::ResourceHandle output,
                 VkExtent2D extent, VkDeviceAddress exposure_buffer = 0,
                 const ExposureSettings& exposure = {}, const TonemapLook& look = {});

private:
    const rhi::Device* device_    = nullptr;   // non-owning
    rhi::GpuAllocator* allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    std::vector<std::uint32_t>     slots_; // reflected binding of each resource
    // One pipeline per operator (TonemapOperator order).
    std::array<rhi::ManagedPipeline, tonemap_operator_names.size()> pipelines_;
    rhi::Sampler                   sampler_;

    // 1x1x1 3D image bound to the LUT slot when no look LUT is set (the slot
    // is a Texture3D; the shader does not sample it then). Imported into the
    // graph each frame, which moves it to SHADER_READ_ONLY_OPTIMAL.
    rhi::GpuImage   fallback_lut_;
    rhi::ImageView  fallback_lut_view_;
    rhi::GpuBuffer  fallback_exposure_buffer_;  // bound when auto-exposure is off
    VkDeviceAddress fallback_exposure_address_ = 0;
    rhi::GpuBuffer  params_;                    // PostParams, host-visible
    VkDeviceAddress params_address_ = 0;

    rhi::DescriptorBuffer frame_descriptor_;   // rebuilt each add_to_graph call
};

} // namespace engine::renderer
