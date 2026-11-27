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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

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

inline constexpr VkFormat      shadow_map_format = VK_FORMAT_D32_SFLOAT;
inline constexpr std::uint32_t shadow_map_size = 2048; // per cascade

// Push-constant block, mirrors PushConstants in shadow.slang (24 bytes).
struct ShadowPushConstants {
    VkDeviceAddress cascades = 0;
    VkDeviceAddress instances = 0;
    std::uint32_t   cascade = 0;
    std::uint32_t   pad = 0;
};
static_assert(sizeof(ShadowPushConstants) == 24, "must match shadow.slang PushConstants");

// This frame's sun shadows as graph resources.
struct CascadedShadows {
    // D32 2D array, one layer per cascade: read with sampled_compute.
    rhi::ResourceHandle shadow_map;
    // GpuCascades: read with buffer_read_compute (lighting) / _vertex.
    rhi::ResourceHandle cascades;
    VkDeviceAddress     cascades_address = 0;

    [[nodiscard]] bool valid() const { return shadow_map.valid() && cascades.valid(); }
};

// Extra casters recorded inside each cascade's rendering (the pass has
// begun dynamic rendering into that layer, viewport and scissor set).
// `cascades` is the GpuCascades buffer address (view_proj per cascade).
using ShadowCasterFn =
    std::function<void(VkCommandBuffer cmd, std::uint32_t cascade, VkDeviceAddress cascades)>;

class ShadowPass {
public:
    [[nodiscard]] static std::expected<ShadowPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    ShadowPass() = default;
    ShadowPass(ShadowPass&&) noexcept = default;
    ShadowPass& operator=(ShadowPass&&) noexcept = default;
    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

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
    [[nodiscard]] VkImage shadow_image() const { return map_.handle(); }

private:
    struct FrameDraw {
        VkBuffer                        vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                        index_buffer = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        std::uint32_t                   first_instance = 0;
        std::uint32_t                   instance_count = 0;
    };
    struct SetupSlots {
        std::uint32_t params = 0;
        std::uint32_t bounds = 0;
        std::uint32_t cascades = 0;
    };

    void record_cascades(VkCommandBuffer cmd, std::uint32_t count) const;

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

    // Cascade fitting (csm_setup.slang).
    rhi::DescriptorSetLayout       setup_layout_;
    rhi::ManagedPipeline           setup_pipeline_;
    SetupSlots                     setup_slots_{};
    rhi::DescriptorBuffer          setup_descriptor_;
    rhi::GpuBuffer                 setup_params_;
    rhi::GpuBuffer                 cascades_;
    VkDeviceAddress                cascades_address_ = 0;

    // The cascaded map: one D32 layer per cascade.
    rhi::GpuImage               map_;
    rhi::ImageView              array_view_;
    std::vector<rhi::ImageView> layer_views_;
};

} // namespace engine::renderer
