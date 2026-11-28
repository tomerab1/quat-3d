#pragma once

// CloudShadowPass — the cloud shadow map (ROADMAP R13.1).
//
// The standard approach for shadows of a volumetric cloud layer (Unreal's
// volumetric cloud shadow map, Frostbite, Decima): one compute pass per frame
// (cloud_shadow.slang) integrates the cloud layer's optical depth along the
// sun direction into a cloud_shadow_resolution² R16F texture — an
// orthographic projection along the sun covering cloud_shadow_extent_m around
// the camera, snapped to whole texels in light space so it does not swim as
// the camera moves. Each texel holds the sun's transmittance through the
// clouds along one line parallel to the sun; every point on that line below
// the cloud base shares it, so the lighting pass (and, later, transparents,
// grass and R13.3's volumetric fog) shadow with one bilinear fetch
// (lib/cloud_shadow_common.slang). The cost does not depend on the screen
// resolution.
//
// The sun's elevation is clamped to cloud_shadow_min_sun_y for the map:
// below it the path through the shell keeps its length limit and the map
// keeps its last shape, so dawn and dusk never pop to full sun. The CPU
// mirror (CloudShadowFrame::texel_point + cloud_reference::
// shadow_transmittance over cloud_shadow_steps) is the reference the tests
// compare texels against. R13.4: the map integrates the volumetric cloud
// model of lib/clouds.slang (its CloudLayerGpu and CloudNoisePass textures),
// the clouds the sky shows.
//
// One instance per frame in flight (its descriptor buffer is rewritten each
// frame).

#include <array>
#include <cstdint>
#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/cloud_reference.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

inline constexpr std::uint32_t cloud_shadow_resolution = 512;
inline constexpr float         cloud_shadow_extent_m = 6144.0F; // 12 m texels
inline constexpr VkFormat      cloud_shadow_format = VK_FORMAT_R16_SFLOAT;
inline constexpr float         cloud_shadow_min_sun_y = 0.0523F; // sin(3 deg), as the shader
inline constexpr int           cloud_shadow_steps = 16;          // CLOUD_SHADOW_MAP_STEPS

// World position -> map uv: (dot(P, u_axis.xyz) + u_axis.w, dot(P, v_axis.xyz)
// + v_axis.w). Mirrors CloudShadowProjection in lib/cloud_shadow_common.slang.
struct CloudShadowProjection {
    glm::vec4 u_axis{0.0F};
    glm::vec4 v_axis{0.0F};

    [[nodiscard]] glm::vec2 uv(const glm::vec3& world) const {
        return {glm::dot(world, glm::vec3(u_axis)) + u_axis.w,
                glm::dot(world, glm::vec3(v_axis)) + v_axis.w};
    }
};

// One frame's light-space frame of the map.
struct CloudShadowFrame {
    glm::vec3 sun{0.0F, 1.0F, 0.0F};   // unit, toward the sun, elevation-clamped
    glm::vec3 right{1.0F, 0.0F, 0.0F}; // light-space axes (orthonormal with sun)
    glm::vec3 up{0.0F, 0.0F, 1.0F};
    glm::vec2 centre{0.0F};             // snapped light-space centre (m)
    glm::vec3 camera{0.0F};
    float     texel_m = cloud_shadow_extent_m / static_cast<float>(cloud_shadow_resolution);

    // `sun_to` points toward the sun (any elevation; clamped).
    [[nodiscard]] static CloudShadowFrame make(const glm::vec3& sun_to, const glm::vec3& camera);
    [[nodiscard]] CloudShadowProjection projection() const;
    // Where texel (x, y)'s line crosses the camera's height (world).
    [[nodiscard]] glm::vec3 texel_point(std::uint32_t x, std::uint32_t y) const;
};

class CloudNoisePass;

// One frame's map in the graph. Invalid when the frame has none (no weather,
// clouds or cloud shadows off): no cloud shadows then.
struct CloudShadowMap {
    rhi::ResourceHandle   texture; // cloud_shadow_format, resolution²
    CloudShadowProjection projection{};

    [[nodiscard]] bool valid() const { return texture.valid(); }
};

class CloudShadowPass {
public:
    [[nodiscard]] static std::expected<CloudShadowPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    CloudShadowPass() = default;
    CloudShadowPass(CloudShadowPass&&) noexcept = default;
    CloudShadowPass& operator=(CloudShadowPass&&) noexcept = default;
    CloudShadowPass(const CloudShadowPass&) = delete;
    CloudShadowPass& operator=(const CloudShadowPass&) = delete;

    [[nodiscard]] bool valid() const { return pipeline_.handle() != VK_NULL_HANDLE; }

    // Declares the map and the compute pass that fills it for the cloud layer
    // `layer` (make_cloud_layer, as the cloud pass draws it; `noise` its
    // baked textures) under the sun `sun_to`, around `camera`. The descriptor
    // and constant buffers are owned by the pass and rewritten by the next
    // call (the frame slot's fence must have been waited).
    [[nodiscard]] std::expected<CloudShadowMap, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const glm::vec3& sun_to, const glm::vec3& camera,
                 const CloudLayerGpu& layer, const CloudNoisePass& noise);

private:
    const rhi::Device* device_ = nullptr;    // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    std::array<std::uint32_t, 7>   slots_{}; // reflected bindings (kResources order)
    rhi::DescriptorBuffer          frame_descriptor_;
    rhi::GpuBuffer                 layer_buffer_; // CloudLayerGpu, host-mapped
};

} // namespace engine::renderer
