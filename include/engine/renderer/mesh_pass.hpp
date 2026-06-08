#pragma once

// MeshPass — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises a list of (mesh, material, transform) draw items into the GBuffer
// (albedo, octahedral world normal, packed metallic/roughness/occlusion, depth)
// using gbuffer.slang. Materials are bound through a per-draw descriptor buffer
// (the IMaterial parameter block from Slice 3.4). There is no ECS yet, so the
// caller supplies an explicit draw list; the lighting pass (3.6) consumes the
// GBuffer this pass produces.

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
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// GBuffer attachment formats. Albedo/material are 8-bit UNORM; the world normal
// is octahedral-encoded into an RG16F target; depth is 32-bit float.
inline constexpr VkFormat gbuffer_albedo_format   = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_normal_format   = VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat gbuffer_material_format = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_depth_format    = VK_FORMAT_D32_SFLOAT;

// Resource handles for the GBuffer targets within a compiled RenderGraph frame.
struct GBufferTargets {
    rhi::ResourceHandle albedo;
    rhi::ResourceHandle normal;
    rhi::ResourceHandle material;
    rhi::ResourceHandle depth;
};

// One thing to draw. `material` may be null — the pass then uses its built-in
// default (white, untextured) material.
struct DrawItem {
    const asset::MeshAsset*     mesh     = nullptr;
    const asset::MaterialAsset* material = nullptr;
    glm::mat4                   model{1.0F};
};

// Push-constant block, mirrors PushConstants in gbuffer.slang (128 bytes).
struct GBufferPushConstants {
    glm::mat4 view_proj{1.0F};
    glm::mat4 model{1.0F};
};
static_assert(sizeof(GBufferPushConstants) == 128, "must match gbuffer.slang PushConstants");

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
    // building one descriptor buffer per draw. The descriptor buffers (and the
    // per-frame draw record) are owned by the pass and live until the next call,
    // so `graph` must be compiled + executed before add_to_graph is called again.
    [[nodiscard]] std::expected<GBufferTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                 std::span<const DrawItem> draws);

private:
    struct FrameDraw {
        VkBuffer                 vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                 index_buffer  = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        glm::mat4                model{1.0F};
        std::uint32_t            descriptor_index = 0;
    };

    [[nodiscard]] VkImageView texture_view_or_fallback(
        const asset::AssetHandle<asset::TextureAsset>& handle) const;

    const rhi::Device*   device_    = nullptr;   // non-owning
    rhi::GpuAllocator*   allocator_ = nullptr;   // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       material_layout_;
    rhi::GraphicsPipeline          pipeline_;

    rhi::Sampler         sampler_;            // shared sampler (binding 6)
    asset::TextureAsset  fallback_texture_;   // 1x1 white, for unbound slots
    asset::MaterialAsset default_material_;   // white, untextured

    // Per-frame, rebuilt each add_to_graph call.
    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_;
};

// Offscreen self-test: render a single triangle with a coloured material into
// the GBuffer, read back the albedo target, and verify the covered centre pixel
// holds the material colour while a corner stays at the clear value.
[[nodiscard]] std::expected<void, core::Error>
run_mesh_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                        const std::string& cooked_shader_dir);

} // namespace engine::renderer
