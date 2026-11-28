#pragma once

// EditSync — keeps loaded terrain tiles in step with an edit list (G19.1).
//
// Each frame the owner hands it the list and the loaded tiles. When the
// list's revision moved, it diffs the stamps against those it last applied
// (changed_regions) and recomputes only those regions on the tiles they
// overlap; consumers then refresh exactly what the result names (the tile
// images, the Jolt height-field blocks, nav regions; scatters notice through
// the tiles' edit cells). A tile that loads is brought up to date whole by
// prepare_tile() before anyone sees it.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain/edits.hpp"
#include "engine/terrain/heightmap.hpp"

namespace engine::terrain {

struct EditTile {
    glm::ivec2 coord{0, 0}; // world tile coordinate (single tile: (0, 0))
    Heightmap* map = nullptr;
};

class EditSync {
public:
    struct TileChange {
        glm::ivec2 coord{0, 0};
        TexelRect  texels{}; // rewritten texels of that tile
    };
    struct Result {
        std::vector<TileChange> tiles;   // tiles whose ground changed
        std::vector<EditRect>   regions; // terrain-local rectangles that changed
        [[nodiscard]] bool empty() const { return regions.empty(); }
    };

    // Applies the difference between the stamps last applied and `stamps`
    // (at `revision`) to `tiles`. Nothing happens while `revision` is the one
    // already applied.
    [[nodiscard]] Result sync(std::span<const TerrainStamp> stamps, std::uint64_t revision,
                              std::span<const EditTile> tiles);

    // Brings a freshly generated or reloaded tile to the current stamps (the
    // last ones passed to sync) and stamps it with their revision. A tile
    // already at that revision (the GPU path applied the same list) is left
    // alone. Returns the rewritten texels.
    TexelRect prepare_tile(const EditTile& tile);

    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] const std::vector<TerrainStamp>& applied() const { return applied_; }

    // Forget the applied list (a new world: every tile reloads anyway).
    void reset() {
        applied_.clear();
        revision_ = 0;
    }

private:
    std::vector<TerrainStamp> applied_;
    std::uint64_t             revision_ = 0;
};

// The frame of a tile held in `map` at world tile `coord`.
[[nodiscard]] inline TileFrame tile_frame(const Heightmap& map, glm::ivec2 coord) {
    return {coord, map.resolution, map.tile_size_m};
}

} // namespace engine::terrain
