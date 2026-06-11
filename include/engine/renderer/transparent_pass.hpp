#pragma once

// TransparentPass — forward pass for alpha-blended and transmissive geometry
// (Phase 7, Slices 7.4/7.5/7.7).
//
// Two pipelines over the deferred HDR result (transparent.slang):
//  - Blend: glTF BLEND materials, depth-tested read-only, SRC_ALPHA blending,
//    two-sided. Draws should be supplied back-to-front.
//  - Transmission: KHR_materials_transmission glass. Per the glTF spec this is
//    NOT alpha blending — surfaces write depth, are back-face culled (unless the
//    material is doubleSided; cull mode is dynamic state) and replace the pixel
//    with refracted scene colour + reflection. The pass snapshots the opaque HDR
//    into a private mip chain so rough transmission can sample a blurred
//    background (mip = f(roughness, ior)), and binds the IBL maps so glass picks
//    up environment reflections.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// Push-constant block, mirrors PushConstants in transparent.slang (192 bytes).
struct TransparentPushConstants {
    glm::mat4 view_proj{1.0F};
    glm::mat4 model{1.0F};
    glm::vec4 camera_pos{0.0F};
    glm::vec4 light_direction{0.0F, 0.0F, 1.0F, 0.0F};
    glm::vec4 light_color{1.0F};
    glm::vec4 ambient{0.0F};
};
static_assert(sizeof(TransparentPushConstants) == 192, "must match transparent.slang PushConstants");

class TransparentPass {
public:
    [[nodiscard]] static std::expected<TransparentPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const rhi::TransferContext& transfer, const std::string& cooked_shader_dir);

    TransparentPass() = default;
    TransparentPass(TransparentPass&&) noexcept = default;
    TransparentPass& operator=(TransparentPass&&) noexcept = default;
    TransparentPass(const TransparentPass&) = delete;
    TransparentPass& operator=(const TransparentPass&) = delete;

    // Render `draws` into `hdr`, depth-tested against `depth` (the opaque GBuffer
    // depth; transmissive draws also write it). `ibl` supplies the environment
    // maps glass reflects — a black fallback is bound when empty. No-op (but
    // still valid) when there are no transparent draws.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, rhi::ResourceHandle depth,
                 VkExtent2D extent, const glm::mat4& view_proj, const DirectionalLightParams& light,
                 const glm::vec3& camera_pos, std::span<const DrawItem> draws,
                 IblViewSet ibl = {});

private:
    // Mip levels of the scene-colour snapshot — how far rough transmission can
    // blur. Must cover the LOD range transparent.slang computes from roughness.
    static constexpr std::uint32_t kSceneColorMips = 6;

    [[nodiscard]] VkImageView texture_view_or_fallback(
        const asset::AssetHandle<asset::TextureAsset>& handle) const;

    // (Re)creates the persistent mipped scene-colour image on extent change.
    [[nodiscard]] std::expected<void, core::Error> ensure_scene_color(VkExtent2D extent);

    struct FrameDraw {
        VkBuffer                        vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                        index_buffer = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        glm::mat4                       model{1.0F};
        std::uint32_t                   descriptor_index = 0;
        bool                            transmissive = false;
        VkCullModeFlags                 cull_mode = VK_CULL_MODE_NONE;
    };

    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       material_layout_;
    rhi::GraphicsPipeline          blend_pipeline_;        // alpha blend, no depth write
    rhi::GraphicsPipeline          transmission_pipeline_; // depth write, dynamic cull
    rhi::Sampler                   sampler_;
    rhi::Sampler                   scene_sampler_; // samples the scene-colour mip chain
    asset::TextureAsset            fallback_texture_;
    asset::MaterialAsset           default_material_;
    IblMaps                        fallback_ibl_;

    // Persistent scene-colour snapshot with a blurred mip chain (rough glass).
    rhi::GpuImage  scene_color_;
    rhi::ImageView scene_color_view_;
    VkExtent2D     scene_color_extent_{0, 0};
    std::uint32_t  scene_color_mips_ = 0;

    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_;
};

} // namespace engine::renderer
