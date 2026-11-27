#include "engine/renderer/terrain_gpu_tiles.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

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
    for (auto& [id, pending] : pending_) pending.promise.set_value(terrain::Heightmap{});
    // cpu_fallbacks_' futures join their workers as they are destroyed.
}

bool GpuTileSource::supports(const terrain::TerrainParams& params) const {
    return synth_.supports(params);
}

std::future<terrain::Heightmap> GpuTileSource::generate(const terrain::TerrainParams& params) {
    const std::uint64_t id = synth_.enqueue(params);
    if (id == 0) return terrain::generate_async(params); // unsupported: CPU
    Pending& p = pending_[id];
    p.params = params;
    return p.promise.get_future();
}

void GpuTileSource::add_to_graph(rhi::RenderGraph& graph) {
    for (auto it = parked_.begin(); it != parked_.end();) {
        it = --it->second.frames_left <= 0 ? parked_.erase(it) : std::next(it);
    }
    std::erase_if(cpu_fallbacks_, [](const std::future<void>& f) {
        return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });

    if (auto r = synth_.add_to_graph(graph); !r) {
        // Only an uncreated pass fails here; hand everything to the CPU.
        std::fprintf(stderr, "[terrain] GPU synthesis failed: %s\n", r.error().message.c_str());
        std::vector<std::uint64_t> ids;
        for (const auto& [id, p] : pending_) ids.push_back(id);
        for (const std::uint64_t id : ids) fall_back_to_cpu(id);
        return;
    }
    for (TerrainSynthPass::Result& result : synth_.collect()) deliver(std::move(result));
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
    parked_.emplace(result.id, Parked{std::move(result.image), unclaimed_frames});
    it->second.promise.set_value(std::move(result.heights));
    pending_.erase(it);
}

void GpuTileSource::fall_back_to_cpu(std::uint64_t id) {
    const auto it = pending_.find(id);
    if (it == pending_.end()) return;
    cpu_fallbacks_.push_back(std::async(
        std::launch::async,
        [promise = std::move(it->second.promise), params = it->second.params]() mutable {
            promise.set_value(terrain::generate(params));
        }));
    pending_.erase(it);
}

std::optional<rhi::GpuImage> GpuTileSource::take_image(std::uint64_t gpu_tile) {
    const auto it = parked_.find(gpu_tile);
    if (it == parked_.end()) return std::nullopt;
    rhi::GpuImage image = std::move(it->second.image);
    parked_.erase(it);
    return image;
}

} // namespace engine::renderer
