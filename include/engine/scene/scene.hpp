#pragma once

// Scene + system scheduler (Phase 4, Slice 4.2).
//
// A Scene owns the entt::registry and runs a fixed, explicit sequence of systems
// each tick. Systems are stateless free functions (per CLAUDE.md); the Scene
// just sequences them and holds the per-tick outputs they produce (the draw
// list). Camera and glTF-graph integration arrive in 4.3 / 4.4.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/core/error.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

class Scene {
public:
    Scene() = default;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

    // Create an entity carrying a Name and an identity Transform — the minimum
    // every scene node has. Components are added by the caller afterwards.
    entt::entity create_entity(std::string name = {});

    // Re-parent `child` under `parent` (entt::null detaches to a root),
    // maintaining the Parent component on the child and the Children list on both
    // the old and new parent.
    void set_parent(entt::entity child, entt::entity parent);

    // Run the fixed system sequence for one tick:
    //   1. TransformSystem    — propagate world matrices down the hierarchy.
    //   2. RenderCollectSystem — gather drawable entities into draw_list().
    void tick();

    // First entity with an active Camera, or entt::null if none.
    [[nodiscard]] entt::entity active_camera() const;

    // Draw list produced by the most recent tick(). Pointers reference assets
    // owned elsewhere (the AssetManager); valid until those assets are released.
    [[nodiscard]] const std::vector<renderer::DrawItem>& draw_list() const noexcept {
        return draw_list_;
    }

private:
    entt::registry                  registry_;
    std::vector<renderer::DrawItem> draw_list_;
};

// ---------------------------------------------------------------------------
// Systems — stateless, exposed for explicit ordering and direct testing.
// ---------------------------------------------------------------------------

// Propagate world-space transforms down the Parent/Children hierarchy:
// world = parent.world * local, starting from roots (entities with no Parent, or
// Parent == entt::null).
void transform_system(entt::registry& registry);

// Gather every entity carrying Transform + MeshRenderer into `out` as DrawItems
// (model = Transform.world, resolved mesh/material pointers). `out` is cleared
// first. Entities with an unloaded/empty mesh are still emitted; the GBuffer
// pass skips draws whose mesh is not renderable.
void render_collect_system(const entt::registry& registry,
                           std::vector<renderer::DrawItem>& out);

// Builds a small hierarchy, ticks the scene, and verifies world-matrix
// propagation and draw-list collection. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_scene_self_test();

} // namespace engine::scene
