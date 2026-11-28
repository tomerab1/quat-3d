#pragma once

// CloudNoisePass — the volumetric clouds' baked noise (ROADMAP R13.4).
//
// Owns the four tileable textures of Schneider's Nubis layout
// (lib/cloud_noise.slang): the 128³ base volume (Perlin-Worley + Worley
// fBm), the 32³ detail volume (Worley fBm), the 128² curl-noise texture and
// the 512² weather-noise texture, all RGBA8 UNORM, baked once by compute at
// creation (blocking) and immutable afterwards (SHADER_READ_ONLY_OPTIMAL),
// so one instance serves every frame in flight. Sampled with sampler()
// (trilinear, REPEAT). No mip chains: at the footprint of a cloud ray step
// (tens to hundreds of metres) the base texels (tile / 128) are never
// minified, and the temporal accumulation filters the detail.
//
// The CPU mirror (cloud_noise.hpp) is the reference the bake is tested
// against.

#include <expected>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

inline constexpr VkFormat cloud_noise_format = VK_FORMAT_R8G8B8A8_UNORM;

class CloudNoisePass {
public:
    [[nodiscard]] static std::expected<CloudNoisePass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    CloudNoisePass() = default;
    CloudNoisePass(CloudNoisePass&&) noexcept = default;
    CloudNoisePass& operator=(CloudNoisePass&&) noexcept = default;
    CloudNoisePass(const CloudNoisePass&) = delete;
    CloudNoisePass& operator=(const CloudNoisePass&) = delete;

    [[nodiscard]] bool valid() const { return base_view_.handle() != VK_NULL_HANDLE; }

    [[nodiscard]] VkImageView base_view() const { return base_view_.handle(); }
    [[nodiscard]] VkImageView detail_view() const { return detail_view_.handle(); }
    [[nodiscard]] VkImageView curl_view() const { return curl_view_.handle(); }
    [[nodiscard]] VkImageView weather_view() const { return weather_view_.handle(); }
    [[nodiscard]] VkSampler   sampler() const { return sampler_.handle(); }
    // Raw images, for readback in tests (SHADER_READ_ONLY_OPTIMAL).
    [[nodiscard]] VkImage base_image() const { return base_.handle(); }
    [[nodiscard]] VkImage detail_image() const { return detail_.handle(); }
    [[nodiscard]] VkImage curl_image() const { return curl_.handle(); }
    [[nodiscard]] VkImage weather_image() const { return weather_.handle(); }

private:
    rhi::GpuImage  base_;
    rhi::ImageView base_view_;
    rhi::GpuImage  detail_;
    rhi::ImageView detail_view_;
    rhi::GpuImage  curl_;
    rhi::ImageView curl_view_;
    rhi::GpuImage  weather_;
    rhi::ImageView weather_view_;
    rhi::Sampler   sampler_;
};

} // namespace engine::renderer
