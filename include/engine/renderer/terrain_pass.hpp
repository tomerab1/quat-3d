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
// Not yet: terrain does not cast into the shadow map (the shadow pass renders
// vertex-buffer meshes only); it receives shadows and IBL like everything else.

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/heightmap.hpp"
#include "engine/terrain/streamer.hpp"

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
    [[nodiscard]] std::expected<void, core::Error>
    set_tile(const glm::ivec2& coord, const terrain::Heightmap& map,
             const rhi::TransferContext& transfer);

    // Retire a tile (streaming unload). No-op for unknown coordinates.
    void remove_tile(const glm::ivec2& coord);

    [[nodiscard]] bool has_tiles() const { return !tiles_.empty(); }

    // Record every loaded tile into the frame. `anchor` is the world position
    // tile (0,0) is centred on (anchor.y offsets all heights); `snowline_m` is
    // the world height where snow takes over. Call once per frame (it also
    // ages the retire queue) and declare AFTER the mesh pass — the chunks
    // render into its GBuffer attachments.
    void add_to_graph(rhi::RenderGraph& graph, const renderer::GBufferTargets& gbuffer,
                      VkExtent2D extent, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                      const glm::vec3& anchor, float snowline_m);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::GraphicsPipeline          pipeline_;
    rhi::Sampler                   sampler_;

    // Shared chunk index buffers, one per LOD (grid triangles + skirt ring).
    struct LodIndices {
        rhi::GpuBuffer buffer;
        std::uint32_t  count = 0;
    };
    std::vector<LodIndices> lods_;

    struct Tile {
        rhi::GpuImage         image;
        rhi::ImageView        view;
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
};

// Offscreen self-test: generate a small tile, upload it, render it over a
// cleared GBuffer from a top-down camera, and verify the centre albedo texel
// is terrain-coloured (the geometry pipeline + VTF + splat actually ran).
[[nodiscard]] std::expected<void, core::Error>
run_terrain_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                           rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                           const std::string& cooked_shader_dir);

} // namespace engine::renderer
