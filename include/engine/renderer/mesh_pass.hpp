#pragma once

// MeshPass — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises instanced (mesh, material) draws into the GBuffer (albedo,
// octahedral world normal, packed metallic/roughness/occlusion, clearcoat,
// emission, velocity, depth) using gbuffer.slang. Materials are bound through
// a descriptor buffer per distinct material (the PbrMaterial parameter block
// from Slice 3.4); per-instance transforms come from the pass's instance
// buffer (R9.1, draw_batching.hpp) and their previous-frame poses from a
// parallel motion buffer (R11, velocity.hpp). The lighting pass (3.6)
// consumes the GBuffer this pass produces.
//
// Pipelines are keyed by GBufferVariant: the material's shader module and its
// alpha mode. glTF MASK materials (asset::material_alpha_mask) use the
// alpha-tested fragment entry point of the built-in program (gbuffer.slang
// fragment_masked, R12.11) and draw after every opaque variant; opaque
// pipelines have no discard and keep early depth testing. A material whose
// `shader` names an authored material shader (ROADMAP A6) draws with that
// shader's program (cooked/shaders/materials/<name>.gbuffer.spv, same
// pipeline layout; it cuts out below alpha 0.5 itself, so it draws with the
// alpha-tested group); until it is compiled the built-in program draws it.

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
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
#include "engine/renderer/instance_cull_pass.hpp"
#include "engine/renderer/material_pipelines.hpp"
#include "engine/renderer/velocity.hpp"
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
// is octahedral-encoded into an RG16F target; emitted radiance is linear HDR
// (RGBA16F, a core colour-attachment format); depth is 32-bit float.
inline constexpr VkFormat gbuffer_albedo_format    = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_normal_format    = VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat gbuffer_material_format  = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gbuffer_clearcoat_format = VK_FORMAT_R8G8_UNORM;
inline constexpr VkFormat gbuffer_emissive_format  = VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat gbuffer_depth_format     = VK_FORMAT_D32_SFLOAT;

// Upper bound on the mips material samplers may reach (covers 32k textures);
// shared by the GBuffer and transparent passes.
inline constexpr std::uint32_t material_sampler_max_lod = 16;

// Colour attachments of every GBuffer writer, in SV_Target order.
inline constexpr std::array<VkFormat, 6> gbuffer_color_formats{
    gbuffer_albedo_format,    gbuffer_normal_format,   gbuffer_material_format,
    gbuffer_clearcoat_format, gbuffer_emissive_format, velocity_format};

// Resource handles for the GBuffer targets within a compiled RenderGraph frame,
// plus the view motion every writer of these targets computes velocity with
// (terrain and grass draw into them after the mesh pass).
struct GBufferTargets {
    rhi::ResourceHandle albedo;
    rhi::ResourceHandle normal;
    rhi::ResourceHandle material;
    rhi::ResourceHandle clearcoat; // r = coat strength, g = coat roughness
    rhi::ResourceHandle emissive;  // rgb = emitted radiance (added by lighting)
    rhi::ResourceHandle velocity;  // velocity_format, velocity.hpp convention
    rhi::ResourceHandle depth;
    ViewMotion          motion{};
};

// How a material is drawn into the GBuffer. `module` names its material
// shader module (empty = the built-in gbuffer.slang program; an authored
// material shader, ROADMAP A6, otherwise); `alpha` picks the opaque or the
// alpha-tested (glTF MASK) entry point of the built-in program.
enum class GBufferAlphaMode : std::uint8_t { opaque, masked };
struct GBufferVariant {
    std::string      module;
    GBufferAlphaMode alpha = GBufferAlphaMode::opaque;

    [[nodiscard]] auto operator<=>(const GBufferVariant&) const = default;
};
[[nodiscard]] GBufferVariant gbuffer_variant(const asset::MaterialAsset& material);

// Push-constant block, mirrors GBufferPushConstants in lib/gbuffer_common.slang
// (192 bytes): the jittered view-projection, last frame's unjittered one, the
// device addresses of the InstanceData and InstanceMotion arrays, this frame's
// jitter, the material clock and the camera (materials' view direction), all
// per pass; and, per draw, the address of last frame's skinned positions
// (InstancedDraw::prev_positions; `has_prev_positions` mirrors it != 0, a null
// test in the shader would need shaderInt64).
struct GBufferPushConstants {
    glm::mat4       view_proj{1.0F};
    glm::mat4       prev_view_proj{1.0F};
    VkDeviceAddress instances = 0;
    VkDeviceAddress motion = 0;
    VkDeviceAddress visible = 0; // the camera's culled list (InstanceCuller)
    glm::vec2       jitter_ndc{0.0F};
    std::uint32_t   has_motion = 0;
    std::uint32_t   has_prev_positions = 0; // per draw
    float           time = 0.0F; // material clock, seconds
    float           pad = 0.0F;
    VkDeviceAddress prev_positions = 0; // per draw
    std::array<std::uint32_t, 2> pad2{}; // camera is 16-byte aligned
    // xyz = camera position, w = 1; orthographic: xyz = unit direction toward
    // the camera, w = 0.
    glm::vec4       camera{0.0F, 0.0F, 0.0F, 1.0F};
};
static_assert(sizeof(GBufferPushConstants) == 208, "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, instances) == 128, "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, visible) == 144, "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, has_prev_positions) == 164,
              "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, time) == 168, "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, prev_positions) == 176,
              "must match gbuffer_common.slang");
static_assert(offsetof(GBufferPushConstants, camera) == 192, "must match gbuffer_common.slang");

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
        VkDeviceAddress          prev_positions = 0; // per-draw push constant
        std::uint32_t            first_instance = 0;
        std::uint32_t            instance_count = 0;
        std::uint32_t            descriptor_index = 0;
        std::uint32_t            draw = 0; // index in the draw list (culler)
        const rhi::ManagedPipeline* pipeline = nullptr; // built-in or material shader
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
    MaterialPipelines              materials_; // authored material shaders, by name

    std::chrono::steady_clock::time_point epoch_{};
    std::optional<float>                  fixed_time_;

    rhi::Sampler         sampler_;            // shared sampler (binding 6)
    float                lod_bias_ = 0.0F;    // the sampler's mipLodBias
    asset::TextureAsset  fallback_texture_;   // 1x1 white, for unbound slots
    asset::MaterialAsset default_material_;   // white, untextured

    // Per-frame, rebuilt each add_to_graph call.
    InstanceBuffer                     instances_;
    InstanceBuffer                     motion_;
    // R6 / R9.4: the camera's GPU instance cull (one view).
    InstanceCuller                     culler_;
    VkDeviceAddress                    frame_instances_ = 0;
    VkDeviceAddress                    frame_motion_ = 0;
    GBufferPushConstants               frame_push_{};
    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
    std::vector<FrameDraw>             frame_draws_; // opaque variants first, then masked
};

} // namespace engine::renderer
