#pragma once

// The frame's volumetrics (ROADMAP R13.3 / R13.4): volumetric clouds v2,
// aerial perspective and froxel fog, from the scene's settings, weather,
// world clock and fog volumes to the passes that draw them. FrameRenderer
// owns one; it builds the cloud layer before the cloud shadow map and
// declares the passes after the lighting pass.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/aerial_perspective_pass.hpp"
#include "engine/renderer/atmospherics_pass.hpp"
#include "engine/renderer/cloud_noise_pass.hpp"
#include "engine/renderer/cloud_reference.hpp"
#include "engine/renderer/fog_pass.hpp"
#include "engine/renderer/fog_settings.hpp"
#include "engine/renderer/volumetric_cloud_pass.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
class RenderGraph;
} // namespace engine::rhi
namespace engine::renderer {
class AtmospherePass;
}

namespace quat::app {

// What the volumetrics read from the frame, after the lighting pass.
struct VolumetricsFrame {
    engine::rhi::ResourceHandle hdr;
    engine::rhi::ResourceHandle depth;
    VkExtent2D                  render_extent{0, 0};
    glm::mat4                   view_proj{1.0F};       // unjittered
    glm::mat4                   jittered_inv_vp{1.0F}; // as the HDR was rendered
    glm::vec3                   camera{0.0F};
    std::uint64_t               frame_number = 0;
    bool                        reset = false; // camera cut / history reset
    const engine::renderer::AtmospherePass* atmosphere = nullptr; // null: no LUT sky
    glm::vec3                   sun_to{0.0F, 1.0F, 0.0F};
    glm::vec3                   sun_radiance{0.0F};  // directional light (scene units)
    glm::vec3                   ambient{0.0F};       // flat ambient
    VkImageView                 irradiance = VK_NULL_HANDLE;
    float                       ibl_intensity = 0.0F;
    const engine::renderer::CascadedShadows* shadows = nullptr;
    const engine::renderer::CloudShadowMap*  cloud_shadow = nullptr;
    const engine::renderer::ClusteredLights* lights = nullptr;
    const engine::renderer::FogSettings*     fog = nullptr;
    const entt::registry*                    registry = nullptr; // FogVolumes, SkyState
};

class VolumetricsRuntime {
public:
    static constexpr std::uint32_t k_slots = 2; // frames in flight

    [[nodiscard]] static std::expected<std::unique_ptr<VolumetricsRuntime>, engine::core::Error>
    create(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
           engine::rhi::PipelineCache& cache, const std::string& shader_dir);

    // Builds this frame's cloud layer (clouds = scene::frame_clouds result;
    // `world_anchored` when it has the weather's world anchor) and the
    // lights it is lit by (the sun, and the moon of the scene's SkyState).
    void begin_frame(const engine::renderer::CloudSettings& clouds, bool world_anchored,
                     const glm::vec3& camera, const glm::vec3& sun_to,
                     const entt::registry* registry);
    [[nodiscard]] const engine::renderer::CloudLayerGpu& cloud_layer() const { return layer_; }
    [[nodiscard]] const engine::renderer::CloudNoisePass& noise() const { return noise_; }

    // Declares aerial perspective, clouds, fog and their composite over
    // `in.hdr` in frame slot `slot`.
    [[nodiscard]] std::expected<void, engine::core::Error>
    add_to_graph(engine::rhi::RenderGraph& graph, std::uint32_t slot, const VolumetricsFrame& in,
                 int march_steps);

private:
    struct Slot {
        engine::renderer::AerialPerspectivePass aerial;
        engine::renderer::VolumetricCloudPass   clouds;
        engine::renderer::FogPass               fog;
        engine::renderer::AtmosphericsPass      composite;
    };

    engine::renderer::CloudNoisePass                  noise_;
    std::shared_ptr<engine::renderer::CloudHistory>   cloud_history_;
    std::shared_ptr<engine::renderer::FogHistory>     fog_history_;
    std::array<Slot, k_slots>                         slots_;
    engine::renderer::CloudLayerGpu                   layer_{};
    engine::renderer::CloudLights                     lights_{};
    // Last frame's view, for the temporal reprojections.
    glm::mat4   prev_view_proj_{1.0F};
    glm::vec3   prev_camera_{0.0F};
    glm::vec2   prev_anchor_km_{0.0F};
    bool        has_prev_ = false;
};

} // namespace quat::app
