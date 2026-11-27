#pragma once

// Grass wiring for the `game` executable (app layer, not engine): finds the
// scene's Grass field, creates the GrassPass on first use (a scene without
// grass pays nothing) and adds generation + draw to each frame's graph.

#include <cstdint>
#include <optional>
#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/renderer/grass.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
struct TransferContext;
} // namespace engine::rhi

namespace engine::renderer {
class TerrainPass;
}

namespace engine::scene {
struct CameraMatrices;
}

namespace quat::app {

// The settings of the scene's grass field: the first Grass entity's.
[[nodiscard]] std::optional<engine::renderer::GrassSettings>
active_grass(const entt::registry& registry);

class GrassRuntime {
public:
    // References must outlive the runtime; `transfer` is used when the pass
    // is first created (inside add_to_graph).
    GrassRuntime(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                 engine::rhi::PipelineCache& cache, const engine::rhi::TransferContext& transfer,
                 std::string shader_dir, std::uint32_t frames_in_flight);

    // Once per frame, after the terrain pass is declared. `slot` is the frame
    // in flight (its previous use has finished); `draw_view_proj` the frame's
    // jittered view-projection (culling uses camera.view_proj); `terrain`
    // null when no terrain is present; `dt` advances the wind clock.
    void add_to_graph(engine::rhi::RenderGraph& graph, std::uint32_t slot,
                      const entt::registry& registry,
                      const engine::renderer::GBufferTargets& gbuffer, VkExtent2D extent,
                      const glm::mat4& draw_view_proj,
                      const engine::scene::CameraMatrices& camera,
                      const engine::renderer::TerrainPass* terrain,
                      const glm::vec3& terrain_anchor, float dt);

private:
    const engine::rhi::Device&          device_;
    engine::rhi::GpuAllocator&          allocator_;
    engine::rhi::PipelineCache&         cache_;
    const engine::rhi::TransferContext& transfer_;
    std::string                         shader_dir_;
    std::uint32_t                       frames_in_flight_;

    std::optional<engine::renderer::GrassPass> pass_;
    bool                                       create_failed_ = false;
    double                                     wind_time_s_ = 0.0;
    std::string                                last_error_;
};

} // namespace quat::app
