#pragma once

// TerrainSynthPass — GPU terrain synthesis (ROADMAP R10.2).
//
// Generates terrain tiles in compute (assets/shaders/terrain_synth.slang):
// the seeded noise field of terrain::generate (FBM, ridged mountains,
// continental mask, iq domain warp) and the grid erosion model (pipe-model
// hydraulic + thermal), in milliseconds instead of the CPU's seconds. Each
// finished tile is copied, on the GPU, into an R32F image left in
// SHADER_READ_ONLY_OPTIMAL for the terrain pass's vertex texture fetch, and
// read back asynchronously into a CPU Heightmap (physics, navmesh, queries)
// that becomes available a frame or two later — no CPU round trip for
// rendering.
//
// The CPU generator stays the reference: the noise matches it to float
// rounding (GPU division / sqrt / fused multiply-add differ), the erosion
// follows src/terrain/grid_erosion.cpp op for op, and tile edges are pure
// noise evaluated on the world texel lattice, so adjacent GPU tiles share
// bit-identical edges. Droplet erosion is serial by construction and stays
// on the CPU: supports() is false for it.
//
// Work is queued with enqueue() and recorded by add_to_graph(), once per
// frame, as render-graph passes: terrain_synth (compute: noise + a budgeted
// slice of erosion steps — long simulations spread over several frames, so
// a tile never stalls one frame), then on a job's last slice
// terrain_synth_upload (buffer -> image) and terrain_synth_publish (image ->
// shader-read layout for vertex + fragment stages, heights -> host-visible
// readback buffer, then a VkEvent the host polls). collect() returns the
// finished jobs. Jobs run one at a time, in order.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/terrain/heightmap.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

class TerrainSynthPass {
public:
    // Largest tile the GPU path synthesises (scratch is ~52 bytes a texel and
    // one float4 field must fit the device's storage-buffer range).
    static constexpr std::uint32_t max_resolution = 2049;
    // Default erosion budget per frame, in texel-steps (≈16 steps of a
    // 1025² tile): a few milliseconds of compute on a desktop GPU.
    static constexpr std::uint64_t default_cell_budget = 16ULL * 1025 * 1025;

    [[nodiscard]] static std::expected<TerrainSynthPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    TerrainSynthPass();
    ~TerrainSynthPass();
    TerrainSynthPass(TerrainSynthPass&&) noexcept;
    TerrainSynthPass& operator=(TerrainSynthPass&&) noexcept;
    TerrainSynthPass(const TerrainSynthPass&) = delete;
    TerrainSynthPass& operator=(const TerrainSynthPass&) = delete;

    [[nodiscard]] bool valid() const { return state_ != nullptr; }

    // True when the GPU path can generate `params`: not droplet erosion
    // (CPU only) and within max_resolution / the device's buffer limits.
    [[nodiscard]] bool supports(const terrain::TerrainParams& params) const;

    // Queues a tile (params.world_tile picks it). Returns the job id (> 0),
    // or 0 when !supports(params).
    [[nodiscard]] std::uint64_t enqueue(const terrain::TerrainParams& params);

    // Erosion texel-steps recorded per add_to_graph (0 = a whole job per
    // call: tests, offline baking). At least one step always runs.
    void set_cell_budget(std::uint64_t cells_per_frame);

    // Records this frame's synthesis work (nothing when idle). Call once per
    // frame, before compiling the graph; also ages released GPU resources.
    // A job whose resources cannot be created fails (collect() reports it)
    // without failing the frame.
    [[nodiscard]] std::expected<void, core::Error> add_to_graph(rhi::RenderGraph& graph);

    struct Result {
        std::uint64_t      id = 0;
        glm::ivec2         tile{0, 0};
        rhi::GpuImage      image;   // R32F metres, SHADER_READ_ONLY_OPTIMAL; null on failure
        terrain::Heightmap heights; // the read-back field; !valid() on failure
    };
    // Jobs whose readback has landed (the GPU signalled their event) or that
    // failed. Non-blocking.
    [[nodiscard]] std::vector<Result> collect();

    // Nothing queued, being recorded, or awaiting its readback.
    [[nodiscard]] bool idle() const;

private:
    struct Job;
    struct State;

    // Heap-held: descriptor buffers point at the layout and entry points.
    std::unique_ptr<State> state_;
};

} // namespace engine::renderer
