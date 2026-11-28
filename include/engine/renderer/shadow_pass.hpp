#pragma once

// ShadowPass — cascaded shadow maps for the sun (Phase 7; 4 cascades in R12).
//
// Per frame: fit up to four cascades to the camera (csm.hpp), then render
// the casters into one layer of a D32 2D-array shadow map per cascade
// (shadow.slang). The lighting pass selects a cascade per pixel and filters
// it (lib/csm.slang: PCF / PCSS, blend band between cascades).
//
// Fitting runs on the GPU by default: csm_setup.slang reads the frame's
// geometry depth bounds from the Hi-Z build (SDSM-style tight near / far, no
// CPU read-back) and writes the cascade buffer the shadow and lighting
// passes read. Without a Hi-Z pyramid, or with CsmSettings::gpu_depth_bounds
// off, the CPU reference (fit_cascades over camera near .. max_distance)
// uploads the same buffer instead — the documented fallback.
//
// Casters: the instanced draw list (meshes and InstancedMesh placements,
// culled on the CPU against caster_cull_view_proj, the volume every cascade
// fits in), drawn into every cascade, plus optional extra casters
// (ShadowCasterFn — e.g. terrain) recorded inside each cascade's rendering.
// The map and the cascade buffer are owned by the pass (one per frame slot)
// and imported into each frame's graph.
//
// Casters are routed by material (R12.11, issues #35 / #23):
//   * opaque: depth only, no fragment work (shadow.slang);
//   * alphaMode MASK: depth with the material's alpha test
//     (shadow_material.slang fragment_masked), so cut-outs let the sun through;
//   * transmissive (glass) and alpha-blended: not in the depth cascades — they
//     multiply their transmittance into a per-cascade translucency map
//     (RGBA16F, rgb = light let through, a = the nearest translucent caster's
//     depth; fragment_translucent), which the lighting pass applies to
//     receivers behind that depth (lib/csm.slang csm_transmittance). Clear
//     glass therefore casts a faint, tinted shadow instead of a black one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/renderer/csm.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/instance_buffer.hpp"
#include "engine/renderer/shadow_ao_settings.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

struct HiZTargets;

inline constexpr std::uint32_t shadow_map_size = 2048; // per cascade

// Shadow pancaking (as in UE / Unity): with depthClamp, casters in front of
// a cascade's near plane clamp to depth 0 instead of being clipped, so each
// cascade's depth range spans only its bounding sphere and D16 is ample
// (sub-millimetre steps in the near cascades; 4 x 2048² x 2 B = 32 MB shared
// by every frame slot). Without depthClamp the range must be padded by the
// caster distance toward the sun, which needs D32.
inline constexpr VkFormat shadow_map_format_pancaked = VK_FORMAT_D16_UNORM;
inline constexpr VkFormat shadow_map_format_padded = VK_FORMAT_D32_SFLOAT;
[[nodiscard]] bool shadow_pancaking(const rhi::Device& device);
[[nodiscard]] VkFormat shadow_map_format(const rhi::Device& device);

// Translucency map per cascade. Half the depth map's resolution (as UE's
// translucent shadows): coloured shadows are soft, and 4 x 1024^2 x 8 B =
// 32 MB, created only once a frame has translucent casters. RGBA16F: the
// depth in alpha needs more than 8 bits.
inline constexpr std::uint32_t translucent_shadow_map_size = 1024;
inline constexpr VkFormat translucent_shadow_format = VK_FORMAT_R16G16B16A16_SFLOAT;

// Push-constant block, mirrors PushConstants in shadow.slang (24 bytes).
struct ShadowPushConstants {
    VkDeviceAddress cascades = 0;
    VkDeviceAddress instances = 0;
    std::uint32_t   cascade = 0;
    std::uint32_t   pad = 0;
};
static_assert(sizeof(ShadowPushConstants) == 24, "must match shadow.slang PushConstants");

// Push-constant block of the casters that read their material, mirrors
// PushConstants in shadow_material.slang (48 bytes).
struct ShadowMaterialPushConstants {
    VkDeviceAddress              cascades = 0;
    VkDeviceAddress              instances = 0;
    glm::vec4                    to_light{0.0F}; // xyz = unit direction toward the sun
    std::uint32_t                cascade = 0;
    std::uint32_t                alpha_hash = 0; // 1: hashed alpha test (R26.3)
    std::array<std::uint32_t, 2> pad{};
};
static_assert(sizeof(ShadowMaterialPushConstants) == 48,
              "must match shadow_material.slang PushConstants");

// This frame's sun shadows as graph resources.
struct CascadedShadows {
    // Depth 2D array, one layer per cascade: read with sampled_compute.
    rhi::ResourceHandle shadow_map;
    // GpuCascades: read with buffer_read_compute (lighting) / _vertex.
    rhi::ResourceHandle cascades;
    VkDeviceAddress     cascades_address = 0;
    // Translucent casters' transmittance, a 2D array like shadow_map
    // (translucent_shadow_format; rgb light let through, a nearest
    // translucent caster depth): read with sampled_compute. Invalid when the
    // frame has no translucent casters.
    rhi::ResourceHandle transmittance;

    [[nodiscard]] bool valid() const { return shadow_map.valid() && cascades.valid(); }
};

// Extra casters recorded inside each cascade's rendering (the pass has
// begun dynamic rendering into that layer, viewport and scissor set).
// `cascades` is the GpuCascades buffer address (view_proj per cascade).
using ShadowCasterFn =
    std::function<void(VkCommandBuffer cmd, std::uint32_t cascade, VkDeviceAddress cascades)>;

// Sun-shadow state shared by every frame slot of one view (like TaaHistory):
// the cascaded shadow map and the GPU fit's range-hysteresis state. Frames
// run in submission order on one queue, so each frame's import waits for
// the previous frame's accesses (the map's lighting reads and cascade
// writes, the state's compute write) instead of keeping one copy per slot.
class CsmShared {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<CsmShared>, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator);

    CsmShared() = default; // empty; use create()
    CsmShared(const CsmShared&) = delete;
    CsmShared& operator=(const CsmShared&) = delete;

    [[nodiscard]] VkImage map() const { return map_.handle(); }
    [[nodiscard]] VkFormat format() const { return format_; }
    // True when casters are pancaked (depth clamp, tight depth range).
    [[nodiscard]] bool pancaked() const { return pancaked_; }
    // Host-visible CsmRangeState (x lo, y hi, z valid) the GPU fit keeps.
    [[nodiscard]] const rhi::GpuBuffer& range_state() const { return range_state_; }
    // The translucency map (null until a frame had translucent casters).
    [[nodiscard]] VkImage translucent_map() const { return translucent_map_.handle(); }

private:
    friend class ShadowPass;

    // Creates the translucency map on first use.
    [[nodiscard]] std::expected<void, core::Error> ensure_translucent(VkDevice device,
                                                                      rhi::GpuAllocator& allocator);

    rhi::GpuImage               map_;
    VkFormat                    format_ = shadow_map_format_pancaked;
    bool                        pancaked_ = true;
    rhi::ImageView              array_view_;
    std::vector<rhi::ImageView> layer_views_;
    rhi::GpuBuffer              range_state_;
    VkDeviceAddress             range_state_address_ = 0;
    rhi::GpuImage               translucent_map_;
    rhi::ImageView              translucent_array_view_;
    std::vector<rhi::ImageView> translucent_layer_views_;
};


class ShadowPass {
public:
    // `shared` is the map + fit state shared by every frame slot; null
    // creates a private one (single-slot use, tests).
    [[nodiscard]] static std::expected<ShadowPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::shared_ptr<CsmShared> shared = nullptr);

    ShadowPass() = default;
    ShadowPass(ShadowPass&&) noexcept = default;
    ShadowPass& operator=(ShadowPass&&) noexcept = default;
    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    // Alpha-tested casters use the GBuffer's hashed coverage when it hashes
    // (R26.3, MeshPass::set_alpha_hash), with a fixed seed.
    void set_alpha_hash(bool hashed) { alpha_hash_ = hashed; }

    // Declares cascade fitting and the cascade rendering into `graph`.
    // `hiz` (may be null) supplies the depth bounds for GPU fitting. Returns
    // invalid handles when `settings.enabled` is false. Uploads
    // `casters.instances` into the pass's instance buffer; it and the draw
    // record are owned by the pass until the next call.
    [[nodiscard]] std::expected<CascadedShadows, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const CsmView& view, const CsmSettings& settings,
                 const HiZTargets* hiz, const InstancedDrawList& casters,
                 std::span<const ShadowCasterFn> extra_casters = {});

    // Convenience: batches `draws` with batch_draws() and draws the result.
    [[nodiscard]] std::expected<CascadedShadows, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const CsmView& view, const CsmSettings& settings,
                 const HiZTargets* hiz, std::span<const DrawItem> draws);

    // The cascade buffer (host-visible: tests read what the GPU fitted).
    [[nodiscard]] const rhi::GpuBuffer& cascade_buffer() const { return cascades_; }
    [[nodiscard]] VkImage shadow_image() const { return shared_->map(); }
    [[nodiscard]] const CsmShared& shared() const { return *shared_; }

private:
    struct FrameDraw {
        VkBuffer                        vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                        index_buffer = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        std::uint32_t                   first_instance = 0;
        std::uint32_t                   instance_count = 0;
        std::uint32_t                   descriptor = 0; // material casters only
    };
    struct SetupSlots {
        std::uint32_t params = 0;
        std::uint32_t bounds = 0;
        std::uint32_t cascades = 0;
        std::uint32_t range_state = 0;
    };

    void record_cascades(VkCommandBuffer cmd, std::uint32_t count) const;

    // Material casters (shadow_casters.cpp).
    [[nodiscard]] std::expected<void, core::Error>
    create_material_casters(rhi::PipelineCache& cache, const std::string& cooked_shader_dir);
    // Files `d` (already validated) under the opaque, masked or translucent
    // casters by its material.
    [[nodiscard]] std::expected<void, core::Error> add_caster(const InstancedDraw& d);
    // The first frame with material casters clears the fallback texture in
    // its graph: returns the imported handle the caster passes then read
    // (invalid on later frames, when it is already initialised).
    [[nodiscard]] rhi::ResourceHandle add_fallback_clear(rhi::RenderGraph& graph);
    // Declares the translucency map's rendering (only when there are
    // translucent casters) and sets out.transmittance.
    [[nodiscard]] std::expected<void, core::Error>
    add_translucent_pass(rhi::RenderGraph& graph, CascadedShadows& out, std::uint32_t count,
                         rhi::ResourceHandle fallback);
    void draw_material_casters(VkCommandBuffer cmd, const rhi::ManagedPipeline& pipeline,
                               std::span<const FrameDraw> draws, std::uint32_t cascade) const;
    void record_translucent(VkCommandBuffer cmd, std::uint32_t count) const;

    const rhi::Device*     device_    = nullptr; // non-owning
    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::GpuAllocator*     allocator_ = nullptr; // non-owning
    // Empty set 0: makes the caster pipeline a descriptor-buffer pipeline, so
    // it may draw after passes that bound descriptor buffers.
    rhi::DescriptorSetLayout empty_layout_;
    rhi::ManagedPipeline   pipeline_;
    InstanceBuffer         instances_;
    VkDeviceAddress        frame_instances_ = 0;
    std::vector<FrameDraw> frame_draws_;
    std::vector<ShadowCasterFn> frame_extra_;

    // Casters reading their material (shadow_material.slang): one
    // descriptor buffer per distinct material per frame, slots from
    // reflection (g_params, g_base_color, g_samp).
    rhi::DescriptorSetLayout           material_layout_;
    std::array<std::uint32_t, 3>       material_slots_{};
    rhi::ManagedPipeline               masked_pipeline_;
    rhi::ManagedPipeline               translucent_pipeline_;
    rhi::Sampler                       material_sampler_;
    // 1x1 white bound for a material without a base-colour texture (never
    // sampled then); cleared by the first frame's graph.
    rhi::GpuImage                      fallback_texture_;
    rhi::ImageView                     fallback_view_;
    bool                               fallback_ready_ = false;
    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::unordered_map<const asset::MaterialAsset*, std::uint32_t> frame_descriptor_of_;
    std::vector<FrameDraw>             frame_masked_;
    std::vector<FrameDraw>             frame_translucent_;
    glm::vec4                          frame_to_light_{0.0F};
    bool                               alpha_hash_ = false;

    // Cascade fitting (csm_setup.slang).
    rhi::DescriptorSetLayout       setup_layout_;
    rhi::ManagedPipeline           setup_pipeline_;
    SetupSlots                     setup_slots_{};
    rhi::DescriptorBuffer          setup_descriptor_;
    rhi::GpuBuffer                 setup_params_;
    rhi::GpuBuffer                 cascades_;
    VkDeviceAddress                cascades_address_ = 0;

    std::shared_ptr<CsmShared>     shared_;
};

} // namespace engine::renderer
