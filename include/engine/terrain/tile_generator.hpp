#pragma once

// TileGenerator — a pluggable asynchronous tile backend (ROADMAP R10.2).
//
// The terrain module is CPU-only; a GPU backend (renderer::GpuTileSource,
// owned by the terrain pass) implements this interface so the streamer and
// the single-tile path can generate on the GPU without depending on the
// renderer. The deterministic CPU generator remains the fallback for
// whatever a backend does not support, and the reference it is tested
// against.

#include <cstdint>
#include <future>
#include <span>
#include <utility>

#include "engine/terrain/edits.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/terrain/heightmap.hpp"

namespace engine::terrain {

class TileGenerator {
public:
    TileGenerator() = default;
    virtual ~TileGenerator() = default;
    TileGenerator(const TileGenerator&) = delete;
    TileGenerator& operator=(const TileGenerator&) = delete;
    TileGenerator(TileGenerator&&) = delete;
    TileGenerator& operator=(TileGenerator&&) = delete;

    // True when this backend can generate `params`.
    [[nodiscard]] virtual bool supports(const TerrainParams& params) const = 0;

    // Starts generating the tile `params` describes (params.world_tile). The
    // future becomes ready once its CPU Heightmap exists. A backend may make
    // progress only from its owner's frame loop (the GPU one does), so never
    // block on the future from that thread. Setting `cancel` (may be null)
    // abandons the request: the backend stops its work and the future becomes
    // ready with an invalid Heightmap (for a frame-driven backend, at its
    // next frame).
    [[nodiscard]] virtual std::future<Heightmap> generate(const TerrainParams& params,
                                                          CancelToken cancel) = 0;

    // Terrain edits (G19.1) a backend applies to the tiles it generates from
    // now on, tagging them with `revision` (Heightmap::edit_revision; tiles
    // generated with other edits get recomputed on the CPU when they land).
    // A backend that cannot apply edits ignores this: the CPU applies them.
    virtual void set_edits(std::span<const TerrainStamp> stamps, std::uint64_t revision) {
        (void)stamps;
        (void)revision;
    }
};

// Generates a tile on `backend` when it supports `params`, else on a CPU
// worker (generate_async) that `cancel` can stop. A null backend means CPU.
[[nodiscard]] inline std::future<Heightmap>
generate_tile_async(TileGenerator* backend, const TerrainParams& params, CancelToken cancel = {}) {
    if (backend != nullptr && backend->supports(params)) {
        return backend->generate(params, std::move(cancel));
    }
    return generate_async(params, std::move(cancel));
}

} // namespace engine::terrain
