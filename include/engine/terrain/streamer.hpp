#pragma once

// TerrainStreamer — tiles of an infinite seeded world around the camera
// (Phase 12, Slice 12.4).
//
// Keeps a (2*radius+1)² window of tiles centred on the camera's tile loaded.
// One generation job runs at a time on a worker thread (nearest-missing-tile
// first), so streaming never floods the CPU; consumers drain ready/removed
// events each frame and own the GPU/physics side. Tiles share one seed — the
// generator samples noise in world-tile coordinates and fades erosion at tile
// borders, so neighbours continue each other exactly (seam-free).

#include <cstdint>
#include <expected>
#include <future>
#include <map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/terrain/generator.hpp"

namespace engine::terrain {

struct TileCoordLess {
    bool operator()(const glm::ivec2& a, const glm::ivec2& b) const {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    }
};

class TerrainStreamer {
public:
    // `base` describes every tile (seed, resolution, size, erosion...);
    // world_tile is overridden per tile. radius 1 keeps a 3x3 window.
    TerrainStreamer() = default;
    TerrainStreamer(TerrainParams base, int radius)
        : base_(base), radius_(glm::max(radius, 0)) {}

    struct Events {
        std::vector<glm::ivec2> ready;   // freshly loaded tiles (maps() has them)
        std::vector<glm::ivec2> removed; // tiles dropped this update
    };

    // Advance streaming for a camera at `local_xz` metres relative to the
    // world anchor (tile (0,0)'s centre). `block` makes generation synchronous
    // — startup warm-up and tests. Call once per frame.
    [[nodiscard]] Events update(glm::vec2 local_xz, bool block = false);

    [[nodiscard]] const std::map<glm::ivec2, Heightmap, TileCoordLess>& maps() const {
        return maps_;
    }
    [[nodiscard]] const TerrainParams& base() const { return base_; }
    [[nodiscard]] int radius() const { return radius_; }

    // World-space minimum corner of a tile, given the anchor's world position.
    [[nodiscard]] glm::vec3 tile_origin(const glm::ivec2& coord, const glm::vec3& anchor) const {
        const float t = base_.tile_size_m;
        return {anchor.x + (static_cast<float>(coord.x) - 0.5F) * t, anchor.y,
                anchor.z + (static_cast<float>(coord.y) - 0.5F) * t};
    }

private:
    TerrainParams base_{};
    int           radius_ = 1;

    std::map<glm::ivec2, Heightmap, TileCoordLess> maps_;
    std::future<Heightmap> job_;
    glm::ivec2             job_coord_{0, 0};
    bool                   job_active_ = false;
};

// Self-test: a blocking streamer fills its window, follows a camera move
// (loading the new column, dropping the far one), and adjacent tiles share
// bit-identical edge heights (the seam guarantee).
[[nodiscard]] std::expected<void, core::Error> run_terrain_streaming_self_test();

} // namespace engine::terrain
