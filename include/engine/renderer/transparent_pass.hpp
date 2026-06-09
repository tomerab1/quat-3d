#pragma once

// TransparentPass — forward alpha-blended pass (Phase 7, Slice 7.4).
//
// Renders alpha-blended draw items over the deferred HDR result (transparent.slang),
// depth-tested against the opaque GBuffer depth without writing depth, blended
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Forward-shades with one directional light +
// ambient via the shared PBR module. Draws should be supplied back-to-front.

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

    // Blend `draws` into `hdr`, depth-tested against `depth` (the opaque GBuffer
    // depth). No-op (but still valid) when there are no transparent draws.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, rhi::ResourceHandle depth,
                 VkExtent2D extent, const glm::mat4& view_proj, const DirectionalLightParams& light,
                 const glm::vec3& camera_pos, std::span<const DrawItem> draws);

private:
    [[nodiscard]] VkImageView texture_view_or_fallback(
        const asset::AssetHandle<asset::TextureAsset>& handle) const;

    struct FrameDraw {
        VkBuffer                        vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                        index_buffer = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        glm::mat4                       model{1.0F};
        std::uint32_t                   descriptor_index = 0;
    };

    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       material_layout_;
    rhi::GraphicsPipeline          pipeline_;
    rhi::Sampler                   sampler_;
    rhi::Sampler                   scene_sampler_; // samples the copied scene colour
    asset::TextureAsset            fallback_texture_;
    asset::MaterialAsset           default_material_;

    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_;
};

} // namespace engine::renderer
