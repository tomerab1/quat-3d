#pragma once

// FogPass — froxel volumetric fog (ROADMAP R13.3; lib/fog.slang).
//
// Wronski 2014 / Hillaire 2015: two compute passes per frame over a
// FOG_X x FOG_Y x FOG_Z camera-aligned froxel grid:
//   fog_scatter   — media (exponential height fog + local FogVolumes),
//                   lit by the directional light through the cascaded and
//                   cloud shadow maps, R10.1's clustered point / spot lights
//                   and the ambient sky, at a per-frame jittered position,
//                   reprojected and blended with last frame's grid;
//   fog_integrate — front-to-back energy-conserving integration into the
//                   volume the atmospherics composite reads per pixel.
// The scattering history is shared by every frame slot (ping-pong); the
// integrated volume belongs to one frame slot.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/cloud_shadow_pass.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

inline constexpr std::uint32_t fog_grid_x = 160; // FOG_X
inline constexpr std::uint32_t fog_grid_y = 90;  // FOG_Y
inline constexpr std::uint32_t fog_grid_z = 64;  // FOG_Z
inline constexpr std::uint32_t fog_max_volumes = 64; // FOG_MAX_VOLUMES
inline constexpr float         fog_depth_offset_m = 2.0F; // FOG_DEPTH_OFFSET_M
// Weight of the current frame in the temporal filter (Hillaire 2015).
inline constexpr float k_fog_temporal_blend = 0.08F;

// Slice spacing of lib/fog.slang: distance at w in [0, 1] and back.
[[nodiscard]] float fog_slice_depth(float w, float far_m);
[[nodiscard]] float fog_slice_w(float depth_m, float far_m);

// One local fog volume (std430, 80 bytes; FogVolumeGpu in lib/fog.slang).
struct FogVolumeGpu {
    glm::vec4 row0{0.0F}; // world -> unit volume, rows
    glm::vec4 row1{0.0F};
    glm::vec4 row2{0.0F};
    glm::vec4 albedo_density{0.0F}; // rgb albedo, w extinction /m
    glm::vec4 params{0.0F};         // shape (0 box, 1 ellipsoid), edge fade, height falloff
};
static_assert(sizeof(FogVolumeGpu) == 80, "must match FogVolumeGpu in lib/fog.slang");

// `world` maps the unit volume ([-1, 1]³ box or unit sphere) to the world.
[[nodiscard]] FogVolumeGpu make_fog_volume(const glm::mat4& world, bool ellipsoid,
                                           float extinction, const glm::vec3& albedo,
                                           float edge_fade, float height_falloff);

// Height fog + lighting the frame's media use (shared with the composite).
struct FogMedia {
    float     extinction = 0.0F;     // /m at and below the base height
    float     height_falloff = 0.02F; // /m above it
    float     base_height_m = 0.0F;
    float     anisotropy = 0.6F;     // Henyey-Greenstein g
    glm::vec3 albedo{0.9F};
    float     max_distance_m = 1000.0F; // froxel grid range
    glm::vec3 sun_to{0.0F, 1.0F, 0.0F};
    glm::vec3 sun_radiance{0.0F}; // directional light radiance (scene units)
    glm::vec3 ambient{0.0F};      // flat isotropic ambient radiance (scene units)
    float     ibl_intensity = 0.0F; // scale of the irradiance cube's ambient (0 = none)
};

struct FogInputs {
    FogMedia                  media{};
    std::vector<FogVolumeGpu> volumes; // at most fog_max_volumes are used
    glm::mat4  inv_view_proj{1.0F};    // without render jitter
    glm::mat4  prev_view_proj{1.0F};
    glm::vec3  camera{0.0F};
    glm::vec3  prev_camera{0.0F};
    std::uint64_t frame_index = 0;
    bool       reset = false;
    float      pre_exposure = 1.0F;
    const CascadedShadows* shadows = nullptr;      // CSM (nullptr / invalid: unshadowed)
    const CloudShadowMap*  cloud_shadow = nullptr; // nullptr / invalid: no cloud shadows
    const ClusteredLights* lights = nullptr;       // nullptr / invalid: no local lights
    VkImageView            irradiance = VK_NULL_HANDLE; // IBL diffuse cube (ambient)
    // The atmosphere's transmittance LUT: the sun reaches the fog through
    // it, as it reaches lit surfaces (null: the LUT sky is off).
    VkImageView            transmittance = VK_NULL_HANDLE;
};

class FogHistory {
public:
    FogHistory(const rhi::Device& device, rhi::GpuAllocator& allocator);
    ~FogHistory();
    FogHistory(const FogHistory&) = delete;
    FogHistory& operator=(const FogHistory&) = delete;

    struct Slot;

private:
    friend class FogPass;
    [[nodiscard]] std::expected<void, core::Error> ensure();

    const rhi::Device*                 device_;    // non-owning
    rhi::GpuAllocator*                 allocator_; // non-owning
    std::vector<std::unique_ptr<Slot>> slots_;
    std::uint64_t                      frame_ = 0;
    bool                               valid_ = false;
};

class FogPass {
public:
    [[nodiscard]] static std::expected<FogPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::shared_ptr<FogHistory> history);

    FogPass();
    ~FogPass();
    FogPass(FogPass&&) noexcept;
    FogPass& operator=(FogPass&&) noexcept;
    FogPass(const FogPass&) = delete;
    FogPass& operator=(const FogPass&) = delete;

    [[nodiscard]] bool valid() const { return scatter_pipeline_.handle() != VK_NULL_HANDLE; }

    // Declares both passes; returns the integrated volume (RGBA16F, rgb
    // in-scattered radiance x pre-exposure, a transmittance).
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const FogInputs& in);

private:
    struct Fallbacks;

    const rhi::Device* device_ = nullptr;    // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning
    std::shared_ptr<FogHistory> history_;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       scatter_layout_;
    rhi::ManagedPipeline           scatter_pipeline_;
    std::vector<std::uint32_t>     scatter_slots_;
    rhi::DescriptorSetLayout       integrate_layout_;
    rhi::ManagedPipeline           integrate_pipeline_;
    std::vector<std::uint32_t>     integrate_slots_;
    rhi::DescriptorBuffer          scatter_descriptor_;
    rhi::DescriptorBuffer          integrate_descriptor_;
    rhi::GpuBuffer                 constants_; // FogFrameGpu
    rhi::GpuBuffer                 volumes_;   // FogVolumeGpu x fog_max_volumes
    std::unique_ptr<Fallbacks>     fallbacks_; // stand-ins for absent shadows / lights
    rhi::Sampler                   linear_sampler_;
    rhi::Sampler                   shadow_sampler_;
};

} // namespace engine::renderer
