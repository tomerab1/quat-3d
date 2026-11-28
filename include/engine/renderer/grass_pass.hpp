#pragma once

// GrassPass — GPU-generated, wind-animated grass around the camera (ROADMAP
// R9.3). Placement, LOD and culling rules are in grass.hpp.
//
// Per frame, up to five graph passes:
//   grass_reset    (transfer) — zero the indirect-argument buffer;
//   grass_ground   (compute)  — grass_ground.slang tints the GBuffer albedo
//                               of the ground the grass grows on (thatch
//                               near the camera, the blades' mean colour
//                               toward the radius and beyond); only with a
//                               GBuffer and a non-zero ground cover;
//   grass_generate (compute)  — grass_generate.slang evaluates every cell of
//                               the camera-centred grid (clump pull, terrain
//                               height and normal, slope / height limits,
//                               distance + pixel-footprint LOD, tile + blade
//                               frustum culling, the ground's albedo under
//                               each root from the GBuffer) and appends the
//                               survivors to a near- and a far-LOD list;
//   grass_finalize (compute)  — clamps the counts to the lists' capacities
//                               and writes two VkDrawIndexedIndirectCommands;
//   grass_draw     (graphics) — grass.slang draws both lists into the GBuffer
//                               (vkCmdDrawIndexedIndirect; blade geometry is
//                               built in the vertex shader from the instance).
// Every buffer is imported into the render graph, so the compute -> indirect
// / vertex hazards are synchronised by its buffer barriers.
//
// Blades receive shadows, IBL and clustered lights through the deferred
// lighting pass like any GBuffer surface, and darken toward their roots
// (occlusion in the GBuffer) instead of casting into the shadow map: the
// lists are culled to the camera frustum, so casting correctly needs a
// second generation against the light's frustum (or the union of both) to
// avoid shadows popping at the screen edges — left for cascaded shadows.
//
// Pipelines are shared; buffers and descriptors exist per frame in flight:
// the caller passes the slot index and must have waited for that slot's
// previous frame (the frame loop's fence wait) before add_to_graph.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/grass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/terrain_height_source.hpp"
#include "engine/renderer/wind_field_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// What one frame's grass needs. The velocity target (R11) gets each blade's
// motion from the wind at `prev_time_s` (unset: no wind motion) — with a wind
// field, in `wind.prev_texture` (last frame's field) — and the view motion of
// the GBuffer it draws into.
struct GrassFrame {
    GrassSettings settings{};
    glm::mat4     draw_view_proj{1.0F}; // the frame's (jittered) view-projection
    glm::mat4     cull_view_proj{1.0F}; // unjittered, for frustum culling
    glm::vec3     camera{0.0F};
    float         time_s = 0.0F;        // wind clock
    std::optional<float> prev_time_s{}; // the wind clock last frame
    std::span<const TerrainHeightTile> terrain{}; // nearest first; the first 4 are used
    // The global wind field the blades bend in (R13.1). Invalid: no wind.
    WindFieldOutput wind{};
    // Projection scale for the pixel-footprint LOD: render height / (2 tan(fov_y / 2)),
    // i.e. pixels per metre at 1 m. 0 disables it (orthographic views, tests).
    float pixels_per_metre = 0.0F;
};

// Pixels per metre at 1 m for a Vulkan perspective `projection` rendered
// `height` pixels tall (0 for an orthographic projection).
[[nodiscard]] float grass_pixels_per_metre(const glm::mat4& projection, std::uint32_t height);

// GPU-side records, for read-back.
struct GrassBladeGpu {
    glm::vec3     root{0.0F};
    std::uint32_t packed = 0; // bits 0-15 shape hash, 16-23 fade, 24-31 width scale
    std::uint32_t clump = 0;  // the clump's hash (GrassCandidate::clump_hash)
    std::uint32_t tint = 0;   // ground albedo under the root: unorm8 sqrt(rgb), a = weight
    std::uint32_t reserved[2]{};
};
static_assert(sizeof(GrassBladeGpu) == 32);

struct GrassArgsGpu {
    VkDrawIndexedIndirectCommand near{};
    VkDrawIndexedIndirectCommand far{};
    std::uint32_t near_count = 0; // raw (before the capacity clamp)
    std::uint32_t far_count = 0;
    // Statistics over the tiles that survived tile culling (radius + frustum):
    std::uint32_t placed = 0;     // blades passing the ground + LOD tests
    std::uint32_t candidates = 0; // cells within the radius
    std::uint32_t pad[2]{};
};
static_assert(sizeof(GrassArgsGpu) == 64);

// One frame's grass buffers in the graph. Invalid when the frame has no grass
// (nothing was declared).
struct GrassOutput {
    rhi::ResourceHandle blades; // GrassBladeGpu[capacity.near + capacity.far]
    rhi::ResourceHandle args;   // GrassArgsGpu
    GrassCapacity       capacity{};
    GrassGrid           grid{};

    [[nodiscard]] bool valid() const { return args.valid(); }
};

class GrassPass {
public:
    [[nodiscard]] static std::expected<GrassPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const rhi::TransferContext& transfer, const std::string& cooked_shader_dir,
           std::uint32_t frames_in_flight);

    GrassPass() = default;
    GrassPass(GrassPass&&) noexcept = default;
    GrassPass& operator=(GrassPass&&) noexcept = default;
    GrassPass(const GrassPass&) = delete;
    GrassPass& operator=(const GrassPass&) = delete;

    // Declares generation, and the ground cover and the draw into `gbuffer`
    // when it is non-null (declare after the passes that fill it — mesh,
    // terrain — so the cover finds the ground, the roots take its colour and
    // the blades depth-test against it). The GBuffer's albedo must carry
    // VK_IMAGE_USAGE_STORAGE_BIT (MeshPass creates it so).
    [[nodiscard]] std::expected<GrassOutput, core::Error>
    add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot, const GrassFrame& frame,
                 const GBufferTargets* gbuffer, VkExtent2D extent);

private:
    struct Slot {
        rhi::GpuBuffer        params; // host-visible GrassParams
        rhi::GpuBuffer        blades; // device-local blade lists
        rhi::GpuBuffer        args;   // device-local GrassArgsGpu
        rhi::DescriptorBuffer generate_set;
        rhi::DescriptorBuffer ground_set;
        rhi::DescriptorBuffer draw_set;
        std::uint32_t         blade_capacity = 0;
    };
    // Heap-held so descriptor buffers' pointers to the layouts stay valid
    // when the pass is moved.
    struct State {
        const rhi::Device*             device = nullptr;
        rhi::GpuAllocator*             allocator = nullptr;
        rhi::DescriptorBufferFunctions fns{};
        rhi::DescriptorSetLayout       generate_layout;
        rhi::DescriptorSetLayout       ground_layout;
        rhi::DescriptorSetLayout       draw_layout;
        rhi::ManagedPipeline           generate;
        rhi::ManagedPipeline           finalize;
        rhi::ManagedPipeline           ground;
        rhi::ManagedPipeline           draw;
        rhi::GpuBuffer                 indices; // near strip, then far strip
        rhi::GpuImage                  flat_height; // 1x1 R32F stand-in for unused tile slots (and depth)
        rhi::ImageView                 flat_height_view;
        rhi::GpuImage                  calm_wind; // 1x1 zero wind for frames without a field
        rhi::ImageView                 calm_wind_view;
        rhi::GpuImage                  no_albedo; // 1x1 RGBA8 stand-in without a GBuffer
        rhi::ImageView                 no_albedo_view;
        rhi::Sampler                   sampler;
        std::vector<Slot>              slots;
    };

    [[nodiscard]] std::expected<void, core::Error> reserve(Slot& slot, std::uint32_t blades);
    [[nodiscard]] std::expected<void, core::Error> create_pipelines(rhi::PipelineCache& cache,
                                                                    const std::string& dir);
    void add_ground_cover(rhi::RenderGraph& graph, Slot& slot, const GBufferTargets& gbuffer,
                          VkExtent2D extent);
    void add_draw(rhi::RenderGraph& graph, Slot& slot, const GBufferTargets& gbuffer,
                  VkExtent2D extent, const GrassFrame& frame, const GrassOutput& out);

    std::unique_ptr<State> state_;
};

} // namespace engine::renderer
