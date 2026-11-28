#pragma once

// TerrainPass — chunked LOD heightmap rendering (Phase 12, Slice 12.2).
//
// Renders a terrain tile into the existing GBuffer right after the mesh pass
// (same attachments, LOAD instead of CLEAR), so the deferred lighting/shadow/
// IBL stack shades terrain like any other geometry. Geometry is vertex-
// texture-fetch: one R32F heightmap texture, no vertex buffers — each chunk
// draws a shared index buffer whose vertex ids decode to a 64x64 grid (plus a
// perimeter skirt ring) in the vertex shader. Five geomipmap LODs share the
// full-resolution vertex ids, so positions are exact at every level and the
// baked skirts hide cracks between neighbouring chunks of different LODs.
// Per-frame CPU work: frustum-cull chunk AABBs and pick each chunk's LOD by
// camera distance.
//
// One instance is shared by both frame slots: tile descriptors are persistent
// and the per-frame draw list is consumed by the execute lambda in the same
// frame it is built. Tiles stream in and out without pipeline stalls — uploads
// create fresh images and unloads retire the old tile until in-flight frames
// have finished with it.
//
// GPU synthesis (R10.2): tile_generator() generates tiles with the
// TerrainSynthPass, recorded by add_to_graph; the GPU writes the very image
// this pass samples, and set_tile() adopts it (Heightmap::gpu_tile) instead
// of uploading the read-back heights again.
//
// Terrain edits (G19.1) reach the pass through the tiles: set_tile uploads a
// map's edited heights and its RGBA8 paint (material weights + grass mask,
// binding 2), which terrain.slang blends over the procedural splat and the
// grass pass reads as a density mask (TerrainHeightTile::paint).
//
// Terrain casts into the sun's cascaded shadow map through shadow_casters()
// (R12): a ShadowCasterFn the ShadowPass records inside every cascade, drawing
// the same chunk geometry at the same LODs with terrain_shadow.slang.

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/terrain_gpu_tiles.hpp"
#include "engine/renderer/terrain_height_source.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/heightmap.hpp"
#include "engine/terrain/streamer.hpp"
#include "engine/terrain/tile_generator.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Quads per chunk side (the shared grid is (chunk_quads+1)² vertices).
inline constexpr std::uint32_t chunk_quads = 64;
// Geomipmap levels: steps 1, 2, 4, 8, 16.
inline constexpr std::uint32_t lod_count = 5;

class TerrainPass {
public:
    [[nodiscard]] static std::expected<TerrainPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const rhi::TransferContext& transfer, const std::string& cooked_shader_dir);

    TerrainPass() = default;
    TerrainPass(TerrainPass&&) noexcept = default;
    TerrainPass& operator=(TerrainPass&&) noexcept = default;
    TerrainPass(const TerrainPass&) = delete;
    TerrainPass& operator=(const TerrainPass&) = delete;

    // Upload a generated tile at grid coordinate `coord` (R32F, metres). Never
    // stalls the pipeline: a fresh image is created and any tile it replaces is
    // retired (destroyed a few frames later, once in-flight frames are done).
    // A GPU-synthesised map (gpu_tile != 0) adopts the image the GPU already
    // wrote; it is uploaded from `heights` only if that image has expired.
    [[nodiscard]] std::expected<void, core::Error>
    set_tile(const glm::ivec2& coord, const terrain::Heightmap& map,
             const rhi::TransferContext& transfer);

    // The GPU tile backend (R10.2) for TerrainStreamer::set_generator /
    // terrain::generate_tile_async; null when GPU synthesis is unavailable
    // (no compute shader, descriptor buffers...). Its tiles are synthesised
    // by add_to_graph, so their futures only progress while frames run.
    [[nodiscard]] terrain::TileGenerator* tile_generator() const { return gpu_tiles_.get(); }

    // Retire a tile (streaming unload). No-op for unknown coordinates.
    void remove_tile(const glm::ivec2& coord);

    // Per-frame upkeep, whether or not terrain draws this frame (call once
    // per frame, before add_to_graph): retires replaced / removed tiles once
    // in-flight frames are done with them, and runs the GPU tile backend's
    // upkeep (cancellations, deliveries, freeing finished jobs' memory).
    void tick();

    // Cancels GPU tiles not yet delivered (GpuTileSource::abandon): their
    // futures resolve to an invalid Heightmap without further frames.
    void abandon_pending_tiles() {
        if (gpu_tiles_) gpu_tiles_->abandon();
    }

    // Tiles to draw, or GPU tiles being synthesised (add_to_graph drives them).
    [[nodiscard]] bool has_tiles() const {
        return !tiles_.empty() || (gpu_tiles_ && gpu_tiles_->busy());
    }

    // The loaded tiles whose XZ footprint overlaps the disc (`centre`,
    // `radius`), nearest first, at most `max_tiles` — for passes that sample
    // the terrain on the GPU (grass). `anchor` as for add_to_graph. The views
    // stay valid until the frame after the tile is removed or replaced.
    [[nodiscard]] std::vector<TerrainHeightTile> height_tiles(const glm::vec2& centre,
                                                              float radius,
                                                              const glm::vec3& anchor,
                                                              std::size_t max_tiles) const;

    // Record every loaded tile into the frame. `anchor` is the world position
    // tile (0,0) is centred on (anchor.y offsets all heights); `snowline_m` is
    // the world height where snow takes over. Call at most once per frame,
    // after tick() (it also records GPU tile synthesis), and declare AFTER
    // the mesh pass — the chunks render into its GBuffer attachments.
    void add_to_graph(rhi::RenderGraph& graph, const renderer::GBufferTargets& gbuffer,
                      VkExtent2D extent, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                      const glm::vec3& anchor, float snowline_m);

    // The loaded tiles as sun-shadow casters for this frame (R12): chunks
    // inside `cull_view_proj` (ShadowPass's caster volume), at the LODs the
    // camera at `camera_pos` sees. Hand the result to ShadowPass::add_to_graph
    // as an extra caster; it is valid for this frame only (like the draw
    // list, it is rebuilt by the next call). Empty when there are no tiles.
    [[nodiscard]] ShadowCasterFn shadow_casters(const glm::mat4& cull_view_proj,
                                                const glm::vec3& camera_pos,
                                                const glm::vec3& anchor);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    rhi::Sampler                   sampler_;
    // 1x1 RGBA8 "no paint, full grass" for tiles without edits.
    rhi::GpuImage                  default_paint_;
    rhi::ImageView                 default_paint_view_;

    // Shared chunk index buffers, one per LOD (grid triangles + skirt ring).
    struct LodIndices {
        rhi::GpuBuffer buffer;
        std::uint32_t  count = 0;
    };
    std::vector<LodIndices> lods_;

    // GPU tile synthesis; heap-held so tile_generator() stays valid on move.
    std::unique_ptr<GpuTileSource> gpu_tiles_;

    struct Tile {
        rhi::GpuImage         image;
        rhi::ImageView        view;
        // Edit paint (G19.1): RGBA8 dirt / gravel / soil / grass mask; null
        // for an unpainted tile, which samples default_paint_ instead.
        rhi::GpuImage         paint;
        rhi::ImageView        paint_view;
        rhi::DescriptorBuffer descriptor;
        std::uint32_t         resolution = 0;
        float                 tile_size_m = 0.0F;
        float                 min_height = 0.0F;
        float                 max_height = 0.0F;
    };
    std::map<glm::ivec2, Tile, terrain::TileCoordLess> tiles_;
    // Unloaded/replaced tiles wait here until in-flight frames finish.
    std::vector<std::pair<int, Tile>> retired_;

    struct ChunkDraw {
        glm::vec4 chunk;  // origin xz, size, skirt depth
        glm::vec4 region; // uv offset, uv scale, metres/texel
        std::uint32_t lod = 0;
    };
    struct TileDraws {
        const Tile*            tile = nullptr; // map nodes are pointer-stable
        std::vector<ChunkDraw> chunks;
    };
    std::vector<TileDraws> frame_draws_; // rebuilt each add_to_graph, consumed same frame

    [[nodiscard]] VkImageView tile_paint_view(const Tile& tile) const {
        return tile.paint_view.handle() != VK_NULL_HANDLE ? tile.paint_view.handle()
                                                          : default_paint_view_.handle();
    }
    // Visible chunks of every tile (frustum `cull_view_proj`) with their LODs
    // for a camera at `camera_pos`.
    void collect_chunks(const glm::mat4& cull_view_proj, const glm::vec3& camera_pos,
                        const glm::vec3& anchor, std::vector<TileDraws>& out) const;
    [[nodiscard]] std::expected<void, core::Error>
    create_shadow_pipeline(rhi::PipelineCache& cache, const std::string& cooked_shader_dir);
    void record_shadow(VkCommandBuffer cmd, std::uint32_t cascade, VkDeviceAddress cascades,
                       float origin_y) const;

    rhi::ManagedPipeline   shadow_pipeline_;     // terrain_shadow.slang, depth only
    std::vector<TileDraws> shadow_draws_;        // rebuilt each shadow_casters call
};

} // namespace engine::renderer
