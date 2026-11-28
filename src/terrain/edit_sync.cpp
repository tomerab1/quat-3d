#include "engine/terrain/edit_sync.hpp"

#include <utility>

namespace engine::terrain {

EditSync::Result EditSync::sync(std::span<const TerrainStamp> stamps, std::uint64_t revision,
                                std::span<const EditTile> tiles) {
    Result out;
    if (revision == revision_) return out;
    out.regions = changed_regions(applied_, stamps);
    applied_.assign(stamps.begin(), stamps.end());
    revision_ = revision;
    for (const EditTile& t : tiles) {
        if (t.map == nullptr || !t.map->valid()) continue;
        const TileFrame frame = tile_frame(*t.map, t.coord);
        const EditRect bounds = frame.bounds();
        TexelRect changed{};
        for (const EditRect& r : out.regions) {
            if (!r.overlaps(bounds)) continue;
            const TexelRect done = apply_edits(*t.map, frame, applied_, texel_rect(frame, r));
            if (done.empty()) continue;
            changed = changed.empty()
                          ? done
                          : TexelRect{std::min(changed.x0, done.x0), std::min(changed.z0, done.z0),
                                      std::max(changed.x1, done.x1), std::max(changed.z1, done.z1)};
        }
        t.map->edit_revision = revision_;
        if (!changed.empty()) out.tiles.push_back({t.coord, changed});
    }
    return out;
}

TexelRect EditSync::prepare_tile(const EditTile& tile) {
    if (tile.map == nullptr || !tile.map->valid()) return {};
    if (tile.map->edit_revision == revision_ && revision_ != 0) return {};
    const TexelRect done = apply_edits(*tile.map, tile_frame(*tile.map, tile.coord), applied_);
    tile.map->edit_revision = revision_;
    return done;
}

} // namespace engine::terrain
