#pragma once

// LightingPass — deferred lighting (Phase 3, Slice 3.6).
//
// A fullscreen compute pass that consumes the GBuffer produced by MeshPass,
// applies the directional light, the clustered punctual lights (R10.1: each
// pixel loops only over its froxel's light list, built by LightCullPass) and
// image-based / flat ambient (lighting.slang), and writes a linear HDR colour
// image. The tonemap pass (3.7) reads that HDR image.
//
// R12: the sun is shadowed by the cascaded shadow map (ShadowPass) and a
// contact-shadow march through the depth buffer; GTAO (GtaoPass) occludes the
// ambient term with bent normals, multi-bounce and specular occlusion. All of
// it arrives through LightingOcclusion; the default lights without any.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_ao_settings.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// Linear HDR colour target format the lighting pass writes (and tonemap reads).
inline constexpr VkFormat hdr_color_format = VK_FORMAT_R16G16B16A16_SFLOAT;

// Directional light parameters (xyz direction TO the light, colour + intensity,
// flat ambient used when the sky / IBL is off).
struct DirectionalLightParams {
    glm::vec4 direction{0.0F, 0.0F, 1.0F, 0.0F}; // xyz = direction TO light
    glm::vec4 color{1.0F, 1.0F, 1.0F, 1.0F};     // rgb colour, w intensity
    glm::vec4 ambient{0.03F, 0.03F, 0.03F, 0.0F};
};

[[nodiscard]] constexpr ContactShadowSettings no_contact_shadows() {
    ContactShadowSettings s;
    s.enabled = false;
    return s;
}

// Shadowing and ambient occlusion inputs of the lighting pass (R12). The
// default lights with neither: sun unshadowed, material occlusion only.
struct LightingOcclusion {
    CascadedShadows       shadows;  // ShadowPass output; invalid = unshadowed sun
    CsmSettings           csm;      // filtering, bias and the cascade debug view
    rhi::ResourceHandle   ao;       // GtaoPass output; invalid = material AO only
    GtaoSettings          gtao;     // bent normals / specular occlusion / multi-bounce
    ContactShadowSettings contact = no_contact_shadows();
    // The camera projection (unjittered; perspective only): linearises the
    // depth buffer for the contact-shadow march.
    glm::mat4             projection{1.0F};
    std::uint32_t         frame_index = 0; // rotates the filter noise for TAA
};

// Lighting terms tuned outside LightingOcclusion (RenderSettings::ibl). The
// sun-shadow filter and biases are CsmSettings (LightingOcclusion::csm).
struct LightingTuning {
    float ibl_intensity = 1.0F; // scales image-based ambient + reflections
};

class LightingPass {
public:
    [[nodiscard]] static std::expected<LightingPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    LightingPass() = default;
    LightingPass(LightingPass&&) noexcept = default;
    LightingPass& operator=(LightingPass&&) noexcept = default;
    LightingPass(const LightingPass&) = delete;
    LightingPass& operator=(const LightingPass&) = delete;

    // Declare the HDR target and the lighting compute pass into `graph`, reading
    // `gbuffer` (including its depth). `inv_view_proj` and `camera_pos` let the
    // shader reconstruct each pixel's world position (for the specular view
    // vector). The descriptor buffer and constants are owned by the pass and
    // live until the next call, so the graph must be compiled + executed
    // before reuse.
    // `occlusion` carries the sun's cascaded shadows, contact shadows and GTAO.
    // `lights` is this frame's LightCullPass output (point/spot lights); the
    // default (invalid) set lights with the directional light + ambient only.
    // `atmosphere_skyview`/`atmosphere_transmittance` (Phase 11.1): when both
    // are set and the sky is enabled, the background uses the physically-based
    // sky-view LUT and the sun is transmittance-tinted; otherwise the
    // procedural sky. `clouds` drives the per-pixel volumetric cloud march
    // (LUT sky only). `tuning` holds the IBL scale.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer, VkExtent2D extent,
                 const DirectionalLightParams& light, const glm::mat4& inv_view_proj,
                 const glm::vec3& camera_pos, const LightingOcclusion& occlusion = {},
                 const ClusteredLights& lights = {}, bool enable_sky = false,
                 IblViewSet ibl = {}, VkImageView atmosphere_skyview = VK_NULL_HANDLE,
                 VkImageView atmosphere_transmittance = VK_NULL_HANDLE,
                 const CloudSettings& clouds = {}, const LightingTuning& tuning = {});

private:
    // Descriptor slots of lighting.slang's resources, from reflection (in
    // the order of kResources in lighting_pass.cpp).
    static constexpr std::size_t k_slot_count = 23;

    const rhi::Device* device_    = nullptr;   // non-owning
    rhi::GpuAllocator* allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    std::array<std::uint32_t, k_slot_count> slots_{};
    rhi::Sampler                   shadow_cmp_sampler_; // hardware PCF taps
    rhi::Sampler                   point_sampler_;      // PCSS blocker search

    rhi::GpuBuffer constants_; // LightingConstants, rewritten each call

    EmptyLightClusters empty_lights_; // bound when no clustered lights are supplied

    IblMaps fallback_ibl_; // bound when no real IBL maps are supplied

    // Bound when no cascaded shadows are supplied (never sampled then).
    rhi::GpuImage  fallback_shadow_;
    rhi::ImageView fallback_shadow_view_;
    rhi::GpuBuffer fallback_cascades_;

    rhi::DescriptorBuffer frame_descriptor_;   // rebuilt each add_to_graph call
};

} // namespace engine::renderer
