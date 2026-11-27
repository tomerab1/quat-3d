#pragma once

// MeshPass — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises instanced (mesh, material) draws into the GBuffer (albedo,
// octahedral world normal, packed metallic/roughness/occlusion, velocity,
// depth) using gbuffer.slang. Materials are bound through a descriptor buffer
// per distinct material (the IMaterial parameter block from Slice 3.4);
// per-instance transforms come from the pass's instance buffer (R9.1,
// draw_batching.hpp) and their previous-frame poses from a parallel motion
// buffer (R11, velocity.hpp). The lighting pass (3.6) consumes the GBuffer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
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
#include "engine/renderer/velocity.hpp"
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
// is octahedral-encoded into an RG16F target; depth is 32-bit float.
inline constexpr VkFormat gbuffer_albedo_format    = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_normal_format    = VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat gbuffer_material_format  = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_clearcoat_format = VK_FORMAT_R8G8_UNORM;
inline constexpr VkFormat gbuffer_depth_format     = VK_FORMAT_D32_SFLOAT;

// Upper bound on the mips material samplers may reach (covers 32k textures);
// shared by the GBuffer and transparent passes.
inline constexpr std::uint32_t material_sampler_max_lod = 16;

// Colour attachments of every GBuffer writer, in SV_Target order.
inline constexpr std::array<VkFormat, 5> gbuffer_color_formats{
    gbuffer_albedo_format, gbuffer_normal_format, gbuffer_material_format,
    gbuffer_clearcoat_format, velocity_format};

// Resource handles for the GBuffer targets within a compiled RenderGraph frame,
// plus the view motion every writer of these targets computes velocity with
// (terrain and grass draw into them after the mesh pass).
struct GBufferTargets {
    rhi::ResourceHandle albedo;
    rhi::ResourceHandle normal;
    rhi::ResourceHandle material;
    rhi::ResourceHandle clearcoat; // r = coat strength, g = coat roughness
    rhi::ResourceHandle velocity;  // velocity_format, velocity.hpp convention
    rhi::ResourceHandle depth;
    ViewMotion          motion{};
};

// Push-constant block, mirrors PushConstants in gbuffer.slang (160 bytes): the
// jittered view-projection, last frame's unjittered one, the device addresses
// of the InstanceData and InstanceMotion arrays, and this frame's jitter.
struct GBufferPushConstants {
    glm::mat4       view_proj{1.0F};
    glm::mat4       prev_view_proj{1.0F};
    VkDeviceAddress instances = 0;
    VkDeviceAddress motion = 0;
    glm::vec2       jitter_ndc{0.0F};
    std::uint32_t   has_motion = 0;
    std::uint32_t   pad = 0;
};
static_assert(sizeof(GBufferPushConstants) == 160, "must match gbuffer.slang PushConstants");
static_assert(offsetof(GBufferPushConstants, instances) == 128,
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
    //
    // `view_proj` is what rasterisation uses (jittered in the frame loop);
    // `motion` turns positions into the velocity target (velocity.hpp).
    [[nodiscard]] std::expected<GBufferTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                 const InstancedDrawList& list, const ViewMotion& motion);

    // Without camera motion or jitter (ViewMotion::still(view_proj)).
    [[nodiscard]] std::expected<GBufferTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                 const InstancedDrawList& list);

    // Convenience: batches `draws` with batch_draws() and draws the result.
    [[nodiscard]] std::expected<GBufferTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                 std::span<const DrawItem> draws);

    // Texture LOD bias for material sampling (texture_lod_bias(): negative
    // when rendering below display resolution). Recreates the material
    // sampler when it changes; call between frames of this pass's slot.
    [[nodiscard]] std::expected<void, core::Error> set_texture_lod_bias(float bias);

private:
    struct FrameDraw {
        VkBuffer                 vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                 index_buffer  = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        std::uint32_t            first_instance = 0;
        std::uint32_t            instance_count = 0;
        std::uint32_t            descriptor_index = 0;
    };

    [[nodiscard]] VkImageView texture_view_or_fallback(
        const asset::AssetHandle<asset::TextureAsset>& handle) const;

    const rhi::Device*   device_    = nullptr;   // non-owning
    rhi::GpuAllocator*   allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       material_layout_;
    rhi::ManagedPipeline           pipeline_;

    rhi::Sampler         sampler_;            // shared sampler (binding 6)
    float                lod_bias_ = 0.0F;    // the sampler's mipLodBias
    asset::TextureAsset  fallback_texture_;   // 1x1 white, for unbound slots
    asset::MaterialAsset default_material_;   // white, untextured

    // Per-frame, rebuilt each add_to_graph call.
    InstanceBuffer                     instances_;
    InstanceBuffer                     motion_;
    VkDeviceAddress                    frame_instances_ = 0;
    VkDeviceAddress                    frame_motion_ = 0;
    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_;
};

} // namespace engine::renderer
