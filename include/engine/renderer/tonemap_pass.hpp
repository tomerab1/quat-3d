#pragma once

// TonemapPass — tonemap + present (Phase 3, Slice 3.7).
//
// A fullscreen graphics pass that samples the linear HDR colour from the
// lighting pass, applies ACES + sRGB (tonemap.slang), and writes an LDR colour
// target — the swapchain image in the live path, or a transient image in the
// self-test. The pipeline bakes the output colour format, so a pass instance is
// tied to the format it was created for.

#include <expected>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
struct TransferContext;
}

namespace engine::renderer {

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

    // Add a fullscreen tonemap of `hdr` into the colour target `output` (which
    // must already be a graph resource — imported swapchain or transient). The
    // descriptor buffer is owned by the pass and lives until the next call.
    // `exposure_buffer` is the device address of the auto-exposure pass's adapted-
    // luminance buffer; pass 0 to disable auto-exposure (exposure multiplier 1).
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, rhi::ResourceHandle output,
                 VkExtent2D extent, VkDeviceAddress exposure_buffer = 0);

private:
    const rhi::Device* device_    = nullptr;   // non-owning
    rhi::GpuAllocator* allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::GraphicsPipeline          pipeline_;
    rhi::Sampler                   sampler_;

    rhi::GpuBuffer  fallback_exposure_buffer_;  // bound when auto-exposure is off
    VkDeviceAddress fallback_exposure_address_ = 0;

    rhi::DescriptorBuffer frame_descriptor_;   // rebuilt each add_to_graph call
};

// Full-chain self-test: render a lit triangle through GBuffer -> lighting ->
// tonemap into a transient LDR image, read it back, and verify the tonemapped
// centre pixel matches the ACES+sRGB of the known lit colour (and corners are
// black). Proves the whole Phase 3 deferred chain end to end.
[[nodiscard]] std::expected<void, core::Error>
run_tonemap_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                           rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                           const std::string& cooked_shader_dir);

} // namespace engine::renderer
