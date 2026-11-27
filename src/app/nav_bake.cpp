#include "app/nav_bake.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/scene/scene.hpp"
#include "engine/terrain/heightmap.hpp"
#include "engine/terrain/streamer.hpp"

namespace quat::app {

std::expected<engine::nav::NavMesh, engine::core::Error>
bake_navmesh(entt::registry& registry, const engine::terrain::TerrainStreamer* streamer,
             const engine::terrain::Heightmap* heightmap, const glm::vec3& anchor) {
    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> indices;
    if (streamer != nullptr) {
        // Streamed world: bake every loaded tile (colliders once).
        bool first = true;
        for (const auto& [coord, map] : streamer->maps()) {
            const glm::vec3 origin = streamer->tile_origin(coord, anchor);
            const glm::vec3 centre(origin.x + map.tile_size_m * 0.5F, anchor.y,
                                   origin.z + map.tile_size_m * 0.5F);
            engine::scene::collect_nav_geometry(registry, &map, centre, verts, indices, first);
            first = false;
        }
        if (first) { // no tiles loaded yet: at least the colliders
            engine::scene::collect_nav_geometry(registry, nullptr, anchor, verts, indices, true);
        }
    } else {
        engine::scene::collect_nav_geometry(
            registry, heightmap != nullptr && heightmap->valid() ? heightmap : nullptr, anchor,
            verts, indices);
    }
    auto built = engine::nav::NavMesh::build(verts, indices);
    if (built) {
        std::fprintf(stderr, "[nav] navmesh baked: %zu tris in, %zu polygons\n",
                     indices.size() / 3, built->polygon_count());
    }
    return built;
}

} // namespace quat::app
