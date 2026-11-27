// Scene-level navmesh baking (Phase 13): gathers the static walkable geometry
// and runs the Recast bake.

#include <cstdint>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/scene/navigation.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

std::expected<nav::NavMesh, core::Error>
bake_scene_navmesh(entt::registry& registry, std::span<const NavTerrainTile> terrain,
                   const nav::NavMeshParams& params) {
    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> indices;
    bool colliders_done = false;
    for (const NavTerrainTile& tile : terrain) {
        if (tile.map == nullptr) continue;
        // Colliders once, with the first tile.
        collect_nav_geometry(registry, tile.map, tile.centre, verts, indices, !colliders_done);
        colliders_done = true;
    }
    if (!colliders_done) {
        collect_nav_geometry(registry, nullptr, glm::vec3(0.0F), verts, indices, true);
    }
    if (indices.empty()) {
        return std::unexpected(
            core::Error{"navmesh: nothing to bake (no terrain and no static box colliders)"});
    }
    return nav::NavMesh::build(verts, indices, params);
}

} // namespace engine::scene
