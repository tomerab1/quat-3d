#pragma once

// AtmospherePass — physically-based sky LUTs (Phase 11, Slice 11.1).
//
// Hillaire 2020: transmittance (256x64) and multiple-scattering (32x32) LUTs
// are computed once at creation (sun-independent); the sky-view LUT (192x108
// lat-long) is recomputed whenever the sun direction changes — a sub-
// millisecond compute recorded as a blocking submit, acceptable until a
// time-of-day system wants it inside the frame graph. The lighting pass (and
// the IBL environment bake) sample the sky-view LUT for the background and the
// transmittance LUT for sun-disc/direct-light tinting.

#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

class AtmospherePass {
public:
    // Builds pipelines + LUT images and computes the static LUTs (blocking).
    [[nodiscard]] static std::expected<AtmospherePass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    AtmospherePass() = default;
    AtmospherePass(AtmospherePass&&) noexcept = default;
    AtmospherePass& operator=(AtmospherePass&&) noexcept = default;
    AtmospherePass(const AtmospherePass&) = delete;
    AtmospherePass& operator=(const AtmospherePass&) = delete;

    // Recomputes the sky-view LUT when `sun_to` (unit, toward the sun) moved
    // since the last call. Blocking compute submit on the graphics queue.
    [[nodiscard]] std::expected<void, core::Error> ensure_skyview(const glm::vec3& sun_to);

    [[nodiscard]] bool valid() const { return skyview_view_.handle() != VK_NULL_HANDLE; }
    [[nodiscard]] VkImageView skyview_view() const { return skyview_view_.handle(); }
    [[nodiscard]] VkImageView transmittance_view() const { return transmittance_view_.handle(); }
    // Raw images for the self-test's readback.
    [[nodiscard]] VkImage skyview_image() const { return skyview_.handle(); }
    [[nodiscard]] VkImage transmittance_image() const { return transmittance_.handle(); }

private:
    const rhi::Device* device_    = nullptr; // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       skyview_layout_;
    rhi::ComputePipeline           skyview_pipeline_;

    rhi::GpuImage  transmittance_;
    rhi::ImageView transmittance_view_;
    rhi::GpuImage  multiscatter_;
    rhi::ImageView multiscatter_view_;
    rhi::GpuImage  skyview_;
    rhi::ImageView skyview_view_;
    rhi::Sampler   sampler_;

    glm::vec3 skyview_sun_{0.0F};
};

// Readback sanity checks on the LUTs: transmittance is reddened at the
// horizon vs the zenith, and the noon sky-view is non-black and blue-dominant.
[[nodiscard]] std::expected<void, core::Error>
run_atmosphere_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
