#pragma once

// ExposurePass — histogram auto-exposure (Phase 8.3).
//
// Builds a 256-bin log-luminance histogram of the HDR image, reduces it to an
// average luminance (excluding black), and eases an adapted-luminance value
// toward it over time (exponential). The tonemap pass reads that value to scale
// exposure = key / adapted-luminance. (Tardif / DICE automatic exposure.)

#include <cstdint>
#include <expected>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

class ExposurePass {
public:
    [[nodiscard]] static std::expected<ExposurePass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    ExposurePass() = default;
    ExposurePass(ExposurePass&&) noexcept = default;
    ExposurePass& operator=(ExposurePass&&) noexcept = default;
    ExposurePass(const ExposurePass&) = delete;
    ExposurePass& operator=(const ExposurePass&) = delete;

    // Update the adapted luminance from `hdr` (advances by `dt` seconds). The
    // adapted-luminance buffer persists across frames; pass its device address
    // (exposure_buffer_address) to TonemapPass::add_to_graph.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent, float dt);

    [[nodiscard]] VkDeviceAddress exposure_buffer_address() const { return exposure_address_; }

    // Current adapted average luminance (the exposure buffer is host-visible).
    [[nodiscard]] float adapted_luminance() const;

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       histogram_layout_; // sampled image, storage buffer
    rhi::DescriptorSetLayout       average_layout_;   // storage buffer, storage buffer
    rhi::ComputePipeline           histogram_pipeline_;
    rhi::ComputePipeline           average_pipeline_;

    rhi::GpuBuffer  histogram_buffer_;  // 256 uint bins (cleared each frame)
    VkDeviceAddress histogram_address_ = 0;
    rhi::GpuBuffer  exposure_buffer_;   // [0] adapted luminance (persists)
    VkDeviceAddress exposure_address_ = 0;

    rhi::DescriptorBuffer histogram_descriptor_;
    rhi::DescriptorBuffer average_descriptor_;
};

// Self-test: a uniform dim HDR image adapts to ~its luminance; a uniform bright
// image then pushes the adapted luminance up. Verifies the histogram measures
// luminance and the temporal adaptation tracks it.
[[nodiscard]] std::expected<void, core::Error>
run_exposure_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
