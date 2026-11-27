#pragma once

// ShadowPass — directional shadow depth map (Phase 7, Slice 7.1).
//
// Renders the draw list from the directional light's point of view into a depth
// map (shadow.slang). The lighting pass samples it (with PCF) to shadow surfaces.
// A single high-resolution map fit to the scene; cascades for large worlds are a
// later refinement. Draws the same instanced form as the GBuffer pass (R9.1,
// including the skinned-vertex override, so animated meshes cast correctly
// posed shadows); per-instance transforms come from the pass's instance buffer.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/instance_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

inline constexpr VkFormat       shadow_map_format = VK_FORMAT_D32_SFLOAT;
inline constexpr std::uint32_t  shadow_map_size = 2048;

// Push-constant block, mirrors PushConstants in shadow.slang (72 bytes): the
// light view-projection and the device address of the InstanceData array.
struct ShadowPushConstants {
    glm::mat4       light_view_proj{1.0F};
    VkDeviceAddress instances = 0;
};
static_assert(sizeof(ShadowPushConstants) == 72, "must match shadow.slang PushConstants");
static_assert(offsetof(ShadowPushConstants, instances) == 64,
              "must match shadow.slang PushConstants");

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

    // Declare the shadow depth target + pass into `graph`, rendering `list` from
    // the light's view. Returns the shadow map resource handle (sampled by the
    // lighting pass). Uploads `list.instances` into the pass's instance buffer;
    // it and the per-frame draw record are owned by the pass until the next call.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const glm::mat4& light_view_proj,
                 const InstancedDrawList& list);

    // Convenience: batches `draws` with batch_draws() and draws the result.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const glm::mat4& light_view_proj,
                 std::span<const DrawItem> draws);

private:
    struct FrameDraw {
        VkBuffer                        vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                        index_buffer = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        std::uint32_t                   first_instance = 0;
        std::uint32_t                   instance_count = 0;
    };

    const rhi::Device*     device_    = nullptr; // non-owning
    rhi::GpuAllocator*     allocator_ = nullptr; // non-owning
    rhi::ManagedPipeline   pipeline_;
    InstanceBuffer         instances_;
    VkDeviceAddress        frame_instances_ = 0;
    std::vector<FrameDraw> frame_draws_;
};

} // namespace engine::renderer
