#include "engine/renderer/terrain_gpu_tiles.hpp"

#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include "engine/terrain/edits.hpp"
#include "engine/terrain/generator.hpp"

namespace engine::renderer {

std::expected<std::unique_ptr<GpuTileSource>, core::Error>
GpuTileSource::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                      rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    auto synth = TerrainSynthPass::create(device, allocator, cache, cooked_shader_dir);
    if (!synth) return std::unexpected(synth.error());
    auto out = std::make_unique<GpuTileSource>(Key{});
    out->synth_ = std::move(*synth);
    return out;
}

GpuTileSource::~GpuTileSource() {
    // Never leave a consumer waiting on a future nobody will fulfil.
    abandon();
    // abandon() cancelled the CPU fallbacks, so cpu_fallbacks_' destructor
    // joins workers that are already stopping.
}

void GpuTileSource::abandon() {
    for (auto& [id, pending] : pending_) {
        synth_.cancel(id);
        pending.promise.set_value(terrain::Heightmap{});
    }
    pending_.clear();
    // Tiles regenerating on the CPU are given up too: their futures resolve
    // invalid as soon as the workers reach a cancellation check (#78).
    cpu_fallbacks_.cancel_all();
}

bool GpuTileSource::supports(const terrain::TerrainParams& params) const {
    return synth_.supports(params);
}

void GpuTileSource::set_edits(std::span<const terrain::TerrainStamp> stamps,
                              std::uint64_t revision) {
    if (!synth_.edits_supported()) return; // the CPU applies them on delivery
    edits_.assign(stamps.begin(), stamps.end());
    edit_revision_ = revision;
}

std::future<terrain::Heightmap> GpuTileSource::generate(const terrain::TerrainParams& params,
                                                        terrain::CancelToken cancel) {
    // The stamps that reach this tile, in order (G19.1).
    TerrainSynthPass::EditRequest edits;
    edits.revision = edit_revision_;
    if (!edits_.empty() && params.resolution >= 2) {
        const terrain::TileFrame frame{params.world_tile, params.resolution, params.tile_size_m};
        const terrain::EditRect bounds = frame.bounds();
        std::vector<terrain::TerrainStamp> reach;
        for (const terrain::TerrainStamp& st : edits_) {
            if (terrain::stamp_bounds(st).overlaps(bounds)) reach.push_back(st);
        }
        edits.packed = terrain::pack_stamps(reach);
    }
    const std::uint64_t id = synth_.enqueue(params, std::move(edits));
    if (id == 0) return terrain::generate_async(params, std::move(cancel)); // unsupported: CPU
    Pending& p = pending_[id];
    p.params = params;
    p.cancel = std::move(cancel);
    return p.promise.get_future();
}

void GpuTileSource::tick() {
    // Requests their owner dropped (a replaced streamer, a superseded tile):
    // free the synthesis queue for the tiles that are still wanted.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.cancel && it->second.cancel->load(std::memory_order_relaxed)) {
            synth_.cancel(it->first);
            it->second.promise.set_value(terrain::Heightmap{});
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    for (TerrainSynthPass::Result& result : synth_.collect()) deliver(std::move(result));
    for (auto it = parked_.begin(); it != parked_.end();) {
        it = --it->second.frames_left <= 0 ? parked_.erase(it) : std::next(it);
    }
    cpu_fallbacks_.reap();
    synth_.tick();
}

void GpuTileSource::add_to_graph(rhi::RenderGraph& graph) {
    if (auto r = synth_.add_to_graph(graph); !r) {
        // Only an uncreated pass fails here; hand everything to the CPU.
        std::fprintf(stderr, "[terrain] GPU synthesis failed: %s\n", r.error().message.c_str());
        std::vector<std::uint64_t> ids;
        for (const auto& [id, p] : pending_) ids.push_back(id);
        for (const std::uint64_t id : ids) fall_back_to_cpu(id);
    }
}

void GpuTileSource::deliver(TerrainSynthPass::Result result) {
    if (!result.heights.valid() || result.image.handle() == VK_NULL_HANDLE) {
        std::fprintf(stderr,
                     "[terrain] GPU synthesis of tile (%d,%d) failed; generating on the CPU\n",
                     result.tile.x, result.tile.y);
        fall_back_to_cpu(result.id);
        return;
    }
    const auto it = pending_.find(result.id);
    if (it == pending_.end()) return;
    result.heights.gpu_tile = result.id;
    parked_.emplace(result.id,
                    Parked{std::move(result.image), std::move(result.paint), unclaimed_frames});
    it->second.promise.set_value(std::move(result.heights));
    pending_.erase(it);
}

void GpuTileSource::fall_back_to_cpu(std::uint64_t id) {
    const auto it = pending_.find(id);
    if (it == pending_.end()) return;
    // The request's token goes with it: cancelling the tile still stops the
    // CPU generation, as it would have stopped the GPU job (#78).
    cpu_fallbacks_.start(it->second.params, std::move(it->second.promise),
                         std::move(it->second.cancel));
    pending_.erase(it);
}

std::optional<rhi::GpuImage> GpuTileSource::take_image(std::uint64_t gpu_tile) {
    const auto it = parked_.find(gpu_tile);
    if (it == parked_.end() || it->second.image.handle() == VK_NULL_HANDLE) return std::nullopt;
    rhi::GpuImage image = std::move(it->second.image);
    if (it->second.paint.handle() == VK_NULL_HANDLE) parked_.erase(it);
    return image;
}

std::optional<rhi::GpuImage> GpuTileSource::take_paint_image(std::uint64_t gpu_tile) {
    const auto it = parked_.find(gpu_tile);
    if (it == parked_.end() || it->second.paint.handle() == VK_NULL_HANDLE) return std::nullopt;
    rhi::GpuImage paint = std::move(it->second.paint);
    parked_.erase(it);
    return paint;
}

} // namespace engine::renderer
