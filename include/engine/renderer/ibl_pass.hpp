#pragma once

// IblBaker / IblMaps — split-sum image-based lighting precompute (Phase 7.6).
//
// A one-time precompute (rebuilt only when the environment changes): bake the
// procedural sky into an environment cubemap, convolve a diffuse irradiance cube,
// generate a prefiltered specular cube mip chain (GGX importance sampling), and
// integrate the BRDF LUT. The deferred lighting pass samples the three resulting
// maps (irradiance, prefiltered, brdf_lut) for its ambient/environment term.

#include <cstdint>
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

// Prefiltered specular cube mip count. The highest roughness samples lod
// (mip_count - 1); must match PREFILTER_MAX_LOD in lighting.slang (= 4.0).
inline constexpr std::uint32_t prefilter_mip_count = 5;

// The precomputed IBL maps the lighting pass samples. Cube sample views for the
// irradiance + prefiltered maps, a 2D view for the BRDF LUT, and one shared
// linear/clamp sampler. Move-only (owns GPU images + views + sampler).
struct IblMaps {
    rhi::GpuImage  irradiance;
    rhi::GpuImage  prefiltered;
    rhi::GpuImage  brdf_lut;
    rhi::ImageView irradiance_view;  // VK_IMAGE_VIEW_TYPE_CUBE
    rhi::ImageView prefiltered_view; // VK_IMAGE_VIEW_TYPE_CUBE, all mips
    rhi::ImageView brdf_lut_view;    // VK_IMAGE_VIEW_TYPE_2D
    rhi::Sampler   sampler;

    [[nodiscard]] bool valid() const { return irradiance_view.handle() != VK_NULL_HANDLE; }

    // A 1x1 black fallback (cubes + LUT cleared to 0), so the lighting pass always
    // has valid descriptors to bind at bindings 8-11 even when IBL is disabled.
    [[nodiscard]] static std::expected<IblMaps, core::Error>
    create_fallback(const rhi::Device& device, rhi::GpuAllocator& allocator);
};

class IblBaker {
public:
    [[nodiscard]] static std::expected<IblBaker, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    IblBaker() = default;
    IblBaker(IblBaker&&) noexcept = default;
    IblBaker& operator=(IblBaker&&) noexcept = default;
    IblBaker(const IblBaker&) = delete;
    IblBaker& operator=(const IblBaker&) = delete;

    // Bake the full IBL set for a sun pointing along `sun_dir` (direction TO the
    // sun). Records all precompute dispatches into a one-time command buffer on the
    // graphics queue and blocks until they finish; the maps are left in
    // SHADER_READ_ONLY_OPTIMAL, ready to sample.
    [[nodiscard]] std::expected<IblMaps, core::Error> bake(const glm::vec3& sun_dir);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       env_layout_;   // b0 storage
    rhi::DescriptorSetLayout       conv_layout_;  // b0 sampled, b1 sampler, b2 storage
    rhi::DescriptorSetLayout       lut_layout_;   // b0 storage
    rhi::ComputePipeline           env_pipeline_;
    rhi::ComputePipeline           irradiance_pipeline_;
    rhi::ComputePipeline           prefilter_pipeline_;
    rhi::ComputePipeline           lut_pipeline_;
    rhi::Sampler                   env_sampler_;
};

// Self-test: bake the IBL set and verify the BRDF LUT, irradiance, and prefiltered
// maps hold plausible values (LUT scale in [0,1], irradiance positive and sky-
// tinted, a mirror reflection matching the sky).
[[nodiscard]] std::expected<void, core::Error>
run_ibl_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
