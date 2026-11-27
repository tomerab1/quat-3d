#include "app/nav_bake.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/navigation.hpp"
#include "engine/scene/scene.hpp"
#include "engine/terrain/heightmap.hpp"
#include "engine/terrain/streamer.hpp"

namespace quat::app {

std::expected<engine::nav::NavMesh, engine::core::Error>
bake_navmesh(entt::registry& registry, const engine::terrain::TerrainStreamer* streamer,
             const engine::terrain::Heightmap* heightmap, const glm::vec3& anchor) {
    // Terrain only while a Terrain entity exists: the heightmap / streamer
    // outlive a deleted terrain, and baking them would add phantom ground
    // (audit N3). Streamed worlds bake every loaded tile.
    std::vector<engine::scene::NavTerrainTile> tiles;
    if (!registry.view<engine::scene::Terrain>().empty()) {
        if (streamer != nullptr) {
            for (const auto& [coord, map] : streamer->maps()) {
                const glm::vec3 origin = streamer->tile_origin(coord, anchor);
                tiles.push_back({&map, glm::vec3(origin.x + map.tile_size_m * 0.5F, anchor.y,
                                                 origin.z + map.tile_size_m * 0.5F)});
            }
        } else if (heightmap != nullptr && heightmap->valid()) {
            tiles.push_back({heightmap, anchor});
        }
    }
    auto built = engine::scene::bake_scene_navmesh(registry, tiles);
    if (built) {
        std::fprintf(stderr, "[nav] navmesh baked: %zu terrain tile(s), %zu polygons\n",
                     tiles.size(), built->polygon_count());
    }
    return built;
}

} // namespace quat::app
