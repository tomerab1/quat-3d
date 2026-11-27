#pragma once

// MeshPass — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises instanced (mesh, material) draws into the GBuffer (albedo,
// octahedral world normal, packed metallic/roughness/occlusion, clearcoat,
// emission, depth) using gbuffer.slang. Materials are bound through a
// descriptor buffer per distinct material (the PbrMaterial parameter block
// from Slice 3.4); per-instance transforms come from the pass's instance
// buffer (R9.1, draw_batching.hpp). A material whose `shader` names an
// authored material shader (ROADMAP A6) draws with that shader's program
// (cooked/shaders/materials/<name>.gbuffer.spv, same pipeline layout); until
// it is compiled the built-in program draws it. The lighting pass (3.6)
// consumes the GBuffer this pass produces.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/instance_buffer.hpp"
#include "engine/renderer/material_pipelines.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// GBuffer attachment formats. Albedo/material are 8-bit UNORM; the world normal
// is octahedral-encoded into an RG16F target; emitted radiance is linear HDR
// (RGBA16F, a core colour-attachment format); depth is 32-bit float.
inline constexpr VkFormat gbuffer_albedo_format    = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_normal_format    = VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat gbuffer_material_format  = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_clearcoat_format = VK_FORMAT_R8G8_UNORM;
inline constexpr VkFormat gbuffer_emissive_format  = VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat gbuffer_depth_format     = VK_FORMAT_D32_SFLOAT;

// Resource handles for the GBuffer targets within a compiled RenderGraph frame.
struct GBufferTargets {
    rhi::ResourceHandle albedo;
    rhi::ResourceHandle normal;
    rhi::ResourceHandle material;
    rhi::ResourceHandle clearcoat; // r = coat strength, g = coat roughness
    rhi::ResourceHandle emissive;  // rgb = emitted radiance (added by lighting)
    rhi::ResourceHandle depth;
};

// Push-constant block, mirrors GBufferPushConstants in lib/gbuffer_common.slang
// (96 bytes): the view-projection, the device address of the InstanceData
// array, the material clock and the camera (materials' view direction).
struct GBufferPushConstants {
    glm::mat4       view_proj{1.0F};
    VkDeviceAddress instances = 0;
    float           time = 0.0F;
    float           pad = 0.0F;
    // xyz = camera position, w = 1; orthographic: xyz = unit direction toward
    // the camera, w = 0.
    glm::vec4       camera{0.0F, 0.0F, 0.0F, 1.0F};
};
static_assert(sizeof(GBufferPushConstants) == 96, "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, instances) == 64, "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, camera) == 80, "must match gbuffer_common.slang");

class MeshPass {
public:
    [[nodiscard]] static std::expected<MeshPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator,
           rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
           const std::string& cooked_shader_dir);

    MeshPass() = default;
    MeshPass(MeshPass&&) noexcept = default;
    MeshPass& operator=(MeshPass&&) noexcept = default;
    MeshPass(const MeshPass&) = delete;
    MeshPass& operator=(const MeshPass&) = delete;

    // Declare the GBuffer transient targets and the gbuffer pass into `graph`,
    // drawing each InstancedDraw of `list` with one instanced draw per submesh.
    // Uploads `list.instances` into the pass's instance buffer and builds one
    // descriptor buffer per distinct material. Those (and the per-frame draw
    // record) are owned by the pass and live until the next call, so `graph`
    // must be compiled + executed before add_to_graph is called again.
    [[nodiscard]] std::expected<GBufferTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                 const InstancedDrawList& list);

    // Convenience: batches `draws` with batch_draws() and draws the result.
    [[nodiscard]] std::expected<GBufferTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                 std::span<const DrawItem> draws);

    // The material clock (MaterialInput::time): seconds since the pass was
    // created, or a fixed value (deterministic captures, tests); nullopt
    // returns to the running clock.
    void set_material_time(std::optional<float> seconds) { fixed_time_ = seconds; }
    [[nodiscard]] float material_time() const;

    // Pipelines of authored material shaders (ROADMAP A6).
    [[nodiscard]] const MaterialPipelines& material_pipelines() const { return materials_; }

private:
    struct FrameDraw {
        VkBuffer                 vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                 index_buffer  = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        std::uint32_t            first_instance = 0;
        std::uint32_t            instance_count = 0;
        std::uint32_t            descriptor_index = 0;
        const rhi::ManagedPipeline* pipeline = nullptr; // built-in or material shader
    };

    [[nodiscard]] VkImageView texture_view_or_fallback(
        const asset::AssetHandle<asset::TextureAsset>& handle) const;

    const rhi::Device*   device_    = nullptr;   // non-owning
    rhi::GpuAllocator*   allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       material_layout_;
    rhi::ManagedPipeline           pipeline_;
    MaterialPipelines              materials_; // authored material shaders, by name

    std::chrono::steady_clock::time_point epoch_{};
    std::optional<float>                  fixed_time_;

    rhi::Sampler         sampler_;            // shared sampler (binding 6)
    asset::TextureAsset  fallback_texture_;   // 1x1 white, for unbound slots
    asset::MaterialAsset default_material_;   // white, untextured

    // Per-frame, rebuilt each add_to_graph call.
    InstanceBuffer                     instances_;
    VkDeviceAddress                    frame_instances_ = 0;
    GBufferPushConstants               frame_push_{};
    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_;
};

} // namespace engine::renderer
