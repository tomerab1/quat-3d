#pragma once

// LightingPass — deferred lighting (Phase 3, Slice 3.6).
//
// A fullscreen compute pass that consumes the GBuffer produced by MeshPass,
// applies a single directional light plus flat ambient (lighting.slang), and
// writes a linear HDR colour image. The tonemap pass (3.7) reads that HDR image.

#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// Linear HDR colour target format the lighting pass writes (and tonemap reads).
inline constexpr VkFormat hdr_color_format = VK_FORMAT_R16G16B16A16_SFLOAT;

// Directional light parameters. Packed into vec4s to mirror LightParams in
// lighting.slang (48-byte push-constant block).
struct DirectionalLightParams {
    glm::vec4 direction{0.0F, 0.0F, 1.0F, 0.0F}; // xyz = direction TO light
    glm::vec4 color{1.0F, 1.0F, 1.0F, 1.0F};     // rgb colour, w intensity
    glm::vec4 ambient{0.03F, 0.03F, 0.03F, 0.0F};
};
static_assert(sizeof(DirectionalLightParams) == 48, "must match lighting.slang LightParams");

// One point light, GPU layout (matches PointLight in lighting.slang).
struct PointLightGpu {
    glm::vec4 position_radius{0.0F, 0.0F, 0.0F, 10.0F}; // xyz position, w radius
    glm::vec4 color_intensity{1.0F, 1.0F, 1.0F, 1.0F};  // rgb colour, w intensity
};
static_assert(sizeof(PointLightGpu) == 32, "must match lighting.slang PointLight");

// Maximum point lights uploaded per frame.
inline constexpr std::uint32_t max_point_lights = 64;

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
    // vector). The descriptor buffer is owned by the pass and lives until the next
    // call, so the graph must be compiled + executed before reuse.
    // `shadow_map` is the directional shadow depth map sampled for shadowing, with
    // `light_view_proj` mapping world space into it; pass an invalid handle (and
    // any matrix) to disable shadows.
    // `atmosphere_skyview`/`atmosphere_transmittance` (Phase 11.1): when both
    // are set and the sky is enabled, the background uses the physically-based
    // sky-view LUT and the sun is transmittance-tinted; otherwise the
    // procedural sky.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer, VkExtent2D extent,
                 const DirectionalLightParams& light, const glm::mat4& inv_view_proj,
                 const glm::vec3& camera_pos, rhi::ResourceHandle shadow_map,
                 const glm::mat4& light_view_proj, std::span<const PointLightGpu> point_lights = {},
                 bool enable_sky = false, const IblMaps* ibl = nullptr,
                 VkImageView atmosphere_skyview = VK_NULL_HANDLE,
                 VkImageView atmosphere_transmittance = VK_NULL_HANDLE);

private:
    const rhi::Device* device_    = nullptr;   // non-owning
    rhi::GpuAllocator* allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ComputePipeline           pipeline_;
    rhi::Sampler                   shadow_sampler_; // samples the shadow map

    rhi::GpuBuffer  point_light_buffer_;     // host-visible, holds up to max_point_lights
    VkDeviceAddress point_light_address_ = 0;

    IblMaps fallback_ibl_; // bound at 8-11 when no real IBL maps are supplied

    rhi::DescriptorBuffer frame_descriptor_;   // rebuilt each add_to_graph call
};

// Offscreen self-test: render the GBuffer (a coloured triangle) then light it,
// read back the HDR target, and verify the lit centre pixel matches the expected
// Lambert result while the background stays black.
[[nodiscard]] std::expected<void, core::Error>
run_lighting_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                            rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                            const std::string& cooked_shader_dir);

// Lights a surface with only a point light (directional + ambient off) and
// verifies it is lit within the light's radius and dark beyond it.
[[nodiscard]] std::expected<void, core::Error>
run_point_light_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                          rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                          const std::string& cooked_shader_dir);

// Bakes IBL and lights a smooth metal with the direct light off: the surface is
// lit purely by the environment reflection, so it must be non-black and sky-
// tinted with IBL enabled, and black with the environment disabled.
[[nodiscard]] std::expected<void, core::Error>
run_ibl_lighting_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                           rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                           const std::string& cooked_shader_dir);

// Renders a fully rough dielectric with and without a smooth clearcoat under a
// head-on light: the coat must add a strong achromatic specular highlight that
// the diffuse base alone cannot produce.
[[nodiscard]] std::expected<void, core::Error>
run_clearcoat_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                        const std::string& cooked_shader_dir);

} // namespace engine::renderer
