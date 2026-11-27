#pragma once

// Navmesh baking for the `game` executable (app layer): gathers the static,
// walkable world — the terrain (streamed tiles or the single editing tile)
// plus static box colliders — and runs Recast. Shared by the editor's
// Physics panel and the remote nav.bake command.

#include <expected>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/nav/navmesh.hpp"

namespace engine::terrain {
class TerrainStreamer;
struct Heightmap;
} // namespace engine::terrain

namespace quat::app {

// `streamer` (streamed world, may be null) takes precedence over `heightmap`
// (single tile, may be null or invalid); `anchor` is the Terrain entity's
// world position. Blocking.
[[nodiscard]] std::expected<engine::nav::NavMesh, engine::core::Error>
bake_navmesh(entt::registry& registry, const engine::terrain::TerrainStreamer* streamer,
             const engine::terrain::Heightmap* heightmap, const glm::vec3& anchor);

} // namespace quat::app
