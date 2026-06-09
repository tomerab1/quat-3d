#pragma once

// ShadowPass — directional shadow depth map (Phase 7, Slice 7.1).
//
// Renders the draw list from the directional light's point of view into a depth
// map (shadow.slang). The lighting pass samples it (with PCF) to shadow surfaces.
// A single high-resolution map fit to the scene; cascades for large worlds are a
// later refinement. Reuses DrawItem (including its skinned-vertex override) so
// animated meshes cast correctly posed shadows.

#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

inline constexpr VkFormat       shadow_map_format = VK_FORMAT_D32_SFLOAT;
inline constexpr std::uint32_t  shadow_map_size = 2048;

// Push-constant block, mirrors PushConstants in shadow.slang (128 bytes).
struct ShadowPushConstants {
    glm::mat4 light_view_proj{1.0F};
    glm::mat4 model{1.0F};
};
static_assert(sizeof(ShadowPushConstants) == 128, "must match shadow.slang PushConstants");

class ShadowPass {
public:
    [[nodiscard]] static std::expected<ShadowPass, core::Error>
    create(const rhi::Device& device, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    ShadowPass() = default;
    ShadowPass(ShadowPass&&) noexcept = default;
    ShadowPass& operator=(ShadowPass&&) noexcept = default;
    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    // Declare the shadow depth target + pass into `graph`, rendering `draws` from
    // the light's view. Returns the shadow map resource handle (sampled by the
    // lighting pass). The per-frame draw record is owned by the pass.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const glm::mat4& light_view_proj,
                 std::span<const DrawItem> draws);

private:
    struct FrameDraw {
        VkBuffer                        vertex_buffer = VK_NULL_HANDLE;
        VkBuffer                        index_buffer = VK_NULL_HANDLE;
        std::span<const asset::SubMesh> submeshes{};
        glm::mat4                       model{1.0F};
    };

    rhi::GraphicsPipeline  pipeline_;
    std::vector<FrameDraw> frame_draws_;
};

} // namespace engine::renderer
