// WorldRuntime's terrain edits (G19.1): the edit list on the Terrain entity
// applied to the loaded tiles, region by region, and the refresh of
// everything built from those tiles.

#include <cstdio>
#include <limits>
#include <span>
#include <vector>

#include "app/world_runtime.hpp"
#include "engine/core/profile.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/terrain_edits.hpp"

namespace quat::app {

namespace {

// Revision handed to EditSync while the terrain has no edit layer: never a
// real list's (those count up from 1).
constexpr std::uint64_t k_no_layer = std::numeric_limits<std::uint64_t>::max();

} // namespace

void WorldRuntime::prepare_edited_tile(glm::ivec2 coord, engine::terrain::Heightmap& map) {
    QUAT_ZONE_N("terrain_edits_prepare");
    if (!terrain_edits_.prepare_tile({coord, &map}).empty()) {
        // An image the GPU synthesised under other edits no longer matches:
        // upload the CPU result instead of adopting it.
        map.gpu_tile = 0;
    }
}

void WorldRuntime::sync_terrain_edits(const engine::scene::Scene& scene) {
    QUAT_ZONE_N("terrain_edits_sync");
    const entt::registry& r = scene.registry();
    std::span<const engine::terrain::TerrainStamp> stamps;
    std::uint64_t revision = k_no_layer;
    const auto view = r.view<const engine::scene::Terrain>();
    if (view.begin() != view.end()) {
        if (const auto* layer = r.try_get<engine::scene::TerrainEditLayer>(*view.begin())) {
            stamps = layer->list.stamps();
            revision = layer->list.revision();
        }
    }
    if (revision == terrain_edits_.revision()) return;

    std::vector<engine::terrain::EditTile> tiles;
    if (terrain_streamer_ && terrain_stream_mode_) {
        for (const auto& [coord, m] : terrain_streamer_->maps()) {
            tiles.push_back({coord, terrain_streamer_->find_map(coord)});
        }
    } else if (terrain_heightmap_.valid() && !terrain_job_.valid()) {
        tiles.push_back({glm::ivec2(0, 0), &terrain_heightmap_});
    }
    const engine::terrain::EditSync::Result changed = terrain_edits_.sync(stamps, revision, tiles);
    if (auto* gen = terrain_pass_.tile_generator()) gen->set_edits(terrain_edits_.applied(), revision);
    if (changed.empty()) return;

    for (const engine::terrain::EditSync::TileChange& t : changed.tiles) {
        engine::terrain::Heightmap* map = terrain_streamer_ && terrain_stream_mode_
                                              ? terrain_streamer_->find_map(t.coord)
                                              : &terrain_heightmap_;
        if (map == nullptr) continue;
        map->gpu_tile = 0; // the CPU now holds the newest heights and paint
        if (auto up = terrain_pass_.set_tile(t.coord, *map, terrain_transfer_); !up) {
            std::fprintf(stderr, "[terrain] edited tile upload failed: %s\n",
                         up.error().message.c_str());
        }
        refresh_tile_body(t.coord, *map, t.texels);
    }

    // Nav regions: a bake is stale only where an edit touched its bounds.
    if (nav_.mesh.valid()) {
        const auto [lo, hi] = nav_.mesh.bounds();
        for (const engine::terrain::EditRect& e : changed.regions) {
            const glm::vec2 a = e.min + glm::vec2(terrain_anchor_.x, terrain_anchor_.z);
            const glm::vec2 b = e.max + glm::vec2(terrain_anchor_.x, terrain_anchor_.z);
            if (a.x <= hi.x && lo.x <= b.x && a.y <= hi.z && lo.z <= b.y) {
                invalidate_navmesh("terrain edited");
                break;
            }
        }
    }
    std::fprintf(stderr, "[terrain] edits applied: %zu region(s), %zu tile(s) refreshed\n",
                 changed.regions.size(), changed.tiles.size());
}

void WorldRuntime::refresh_tile_body(glm::ivec2 coord, const engine::terrain::Heightmap& map,
                                     const engine::terrain::TexelRect& texels) {
    if (!play_physics_) return;
    using engine::physics::PhysicsWorld;
    const bool streamed = terrain_streamer_ && terrain_stream_mode_;
    std::uint32_t* body = nullptr;
    if (streamed) {
        const auto it = terrain_bodies_.find(coord);
        if (it != terrain_bodies_.end()) body = &it->second;
    } else if (terrain_body_ != PhysicsWorld::invalid_body) {
        body = &terrain_body_;
    }
    if (body == nullptr) return;
    if (engine::scene::update_height_field_body(*play_physics_, *body, map, texels)) return;
    // Beyond the field's reserved range: replace the body.
    play_physics_->remove_body(*body);
    const float half = map.tile_size_m * 0.5F;
    const glm::vec3 origin = streamed ? terrain_streamer_->tile_origin(coord, terrain_anchor_)
                                      : terrain_anchor_ - glm::vec3(half, 0.0F, half);
    *body = engine::scene::add_height_field_body(*play_physics_, map, origin);
    std::fprintf(stderr, "[terrain] tile (%d,%d) height field rebuilt after an edit\n", coord.x,
                 coord.y);
}

} // namespace quat::app
