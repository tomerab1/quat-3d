#pragma once

// TileGenerator — a pluggable asynchronous tile backend (ROADMAP R10.2).
//
// The terrain module is CPU-only; a GPU backend (renderer::GpuTileSource,
// owned by the terrain pass) implements this interface so the streamer and
// the single-tile path can generate on the GPU without depending on the
// renderer. The deterministic CPU generator remains the fallback for
// whatever a backend does not support, and the reference it is tested
// against.

#include <future>
#include <utility>

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
    // block on the future from that thread.
    [[nodiscard]] virtual std::future<Heightmap> generate(const TerrainParams& params) = 0;
};

// Generates a tile on `backend` when it supports `params`, else on a CPU
// worker (generate_async) that `cancel` can stop. A null backend means CPU.
[[nodiscard]] inline std::future<Heightmap>
generate_tile_async(TileGenerator* backend, const TerrainParams& params, CancelToken cancel = {}) {
    if (backend != nullptr && backend->supports(params)) return backend->generate(params);
    return generate_async(params, std::move(cancel));
}

} // namespace engine::terrain
