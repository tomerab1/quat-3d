#pragma once

// MeshPass — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises instanced (mesh, material) draws into the GBuffer (albedo,
// octahedral world normal, packed metallic/roughness/occlusion, depth) using
// gbuffer.slang. Materials are bound through a descriptor buffer per distinct
// material (the IMaterial parameter block from Slice 3.4); per-instance
// transforms come from the pass's instance buffer (R9.1, draw_batching.hpp).
// The lighting pass (3.6) consumes the GBuffer this pass produces.
//
// Pipelines are keyed by GBufferVariant: the material's shader module and its
// alpha mode. glTF MASK materials (asset::material_alpha_mask) use the
// alpha-tested fragment entry point (gbuffer.slang fragment_masked, R12.11)
// and draw after every opaque variant; opaque pipelines have no discard and
// keep early depth testing. Each module gets one pipeline per alpha mode.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
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
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
struct LoadedShader;
}

namespace engine::renderer {

// GBuffer attachment formats. Albedo/material are 8-bit UNORM; the world normal
// is octahedral-encoded into an RG16F target; depth is 32-bit float.
inline constexpr VkFormat gbuffer_albedo_format    = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_normal_format    = VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat gbuffer_material_format  = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_clearcoat_format = VK_FORMAT_R8G8_UNORM;
inline constexpr VkFormat gbuffer_depth_format     = VK_FORMAT_D32_SFLOAT;

// Upper bound on the mips material samplers may reach (covers 32k textures);
// shared by the GBuffer and transparent passes.
inline constexpr std::uint32_t material_sampler_max_lod = 16;

// Resource handles for the GBuffer targets within a compiled RenderGraph frame.
struct GBufferTargets {
    rhi::ResourceHandle albedo;
    rhi::ResourceHandle normal;
    rhi::ResourceHandle material;
    rhi::ResourceHandle clearcoat; // r = coat strength, g = coat roughness
    rhi::ResourceHandle depth;
};

// How a material is drawn into the GBuffer. `module` names its material
// shader module (empty = the built-in gbuffer.slang program; authored
// material modules, ROADMAP A6, add their own); `alpha` picks the opaque or
// the alpha-tested (glTF MASK) entry point of that module.
enum class GBufferAlphaMode : std::uint8_t { opaque, masked };
struct GBufferVariant {
    std::string      module;
    GBufferAlphaMode alpha = GBufferAlphaMode::opaque;

    [[nodiscard]] auto operator<=>(const GBufferVariant&) const = default;
};
[[nodiscard]] GBufferVariant gbuffer_variant(const asset::MaterialAsset& material);

// Push-constant block, mirrors PushConstants in gbuffer.slang (72 bytes):
// the view-projection and the device address of the InstanceData array.
struct GBufferPushConstants {
    glm::mat4       view_proj{1.0F};
    VkDeviceAddress instances = 0;
};
static_assert(sizeof(GBufferPushConstants) == 72, "must match gbuffer.slang PushConstants");
static_assert(offsetof(GBufferPushConstants, instances) == 64,
              "must match gbuffer.slang PushConstants");

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

private:
    struct FrameDraw {
        VkBuffer                 vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                 index_buffer  = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        std::uint32_t            first_instance = 0;
        std::uint32_t            instance_count = 0;
        std::uint32_t            descriptor_index = 0;
        const rhi::ManagedPipeline* pipeline = nullptr;
        GBufferAlphaMode         alpha = GBufferAlphaMode::opaque;
    };

    // Creates the pipelines of every alpha mode of material module `module`
    // (its program `shader`), from the pass's shared pipeline state.
    [[nodiscard]] std::expected<void, core::Error>
    create_variants(rhi::PipelineCache& cache, const std::string& module,
                    const rhi::LoadedShader& shader);

    [[nodiscard]] VkImageView texture_view_or_fallback(
        const asset::AssetHandle<asset::TextureAsset>& handle) const;

    const rhi::Device*   device_    = nullptr;   // non-owning
    rhi::GpuAllocator*   allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       material_layout_;
    std::map<GBufferVariant, rhi::ManagedPipeline> pipelines_;

    rhi::Sampler         sampler_;            // shared sampler (binding 6)
    asset::TextureAsset  fallback_texture_;   // 1x1 white, for unbound slots
    asset::MaterialAsset default_material_;   // white, untextured

    // Per-frame, rebuilt each add_to_graph call.
    InstanceBuffer                     instances_;
    VkDeviceAddress                    frame_instances_ = 0;
    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_; // opaque variants first, then masked
};

} // namespace engine::renderer
