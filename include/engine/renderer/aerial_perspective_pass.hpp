#pragma once

// AerialPerspectivePass — Hillaire 2020's camera-frustum aerial perspective
// volume (ROADMAP R13.3; lib/aerial_perspective.slang, atmos_aerial.slang).
//
// Each frame one small compute pass integrates the atmosphere's
// in-scattering and transmittance along the view rays of a 32x32 screen
// grid into 32 depth slices (quadratically spaced to 96 km), for the sun
// and the moon. The atmospherics composite applies it to opaque pixels at
// their distance (distant terrain takes the haze of the air in front of
// it), and the cloud march to its clouds. One instance per frame in flight:
// the volume is rewritten every frame and read only within it.

#include <array>
#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

inline constexpr std::uint32_t aerial_perspective_size = 32;   // AP_SIZE
inline constexpr std::uint32_t aerial_perspective_slices = 32; // AP_SLICES
inline constexpr float         aerial_perspective_max_km = 96.0F;

struct AerialPerspectiveInputs {
    glm::mat4   inv_view_proj{1.0F}; // clip -> world, without the render jitter
    glm::vec3   sun_to{0.0F, 1.0F, 0.0F};
    float       sun_irradiance = 1.0F; // relative to the reference sun
    glm::vec3   moon_to{0.0F, -1.0F, 0.0F};
    float       moon_irradiance = 0.0F;
    VkImageView transmittance = VK_NULL_HANDLE; // AtmospherePass LUTs
    VkImageView multiscatter = VK_NULL_HANDLE;
};

class AerialPerspectivePass {
public:
    [[nodiscard]] static std::expected<AerialPerspectivePass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    AerialPerspectivePass() = default;
    AerialPerspectivePass(AerialPerspectivePass&&) noexcept = default;
    AerialPerspectivePass& operator=(AerialPerspectivePass&&) noexcept = default;
    AerialPerspectivePass(const AerialPerspectivePass&) = delete;
    AerialPerspectivePass& operator=(const AerialPerspectivePass&) = delete;

    [[nodiscard]] bool valid() const { return pipeline_.handle() != VK_NULL_HANDLE; }

    // Declares the volume (RGBA16F, size² x slices; rgb in-scattering per
    // unit sun irradiance, a transmittance) and the pass that fills it.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const AerialPerspectiveInputs& in);

    [[nodiscard]] VkImage volume_image() const { return volume_.handle(); }

private:
    const rhi::Device* device_ = nullptr;    // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    std::array<std::uint32_t, 4>   slots_{};
    rhi::DescriptorBuffer          frame_descriptor_;
    rhi::GpuImage                  volume_;
    rhi::ImageView                 volume_view_;
    rhi::Sampler                   sampler_;
};

} // namespace engine::renderer
