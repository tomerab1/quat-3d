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
// One instance is shared by both frame slots: the heightmap descriptor is
// persistent and the per-frame draw list is consumed by the execute lambda in
// the same frame it is built. set_heightmap() waits for the device to idle
// before replacing the texture (regeneration is an explicit editor action).
//
// Not yet: terrain does not cast into the shadow map (the shadow pass renders
// vertex-buffer meshes only); it receives shadows and IBL like everything else.

#include <cstdint>
#include <expected>
#include <string>
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

    // Upload a freshly generated heightmap (R32F, metres). Blocks until the
    // device idles first — in-flight frames may still sample the old texture.
    [[nodiscard]] std::expected<void, core::Error>
    set_heightmap(const terrain::Heightmap& map, const rhi::TransferContext& transfer);

    [[nodiscard]] bool has_heightmap() const { return heightmap_view_.handle() != VK_NULL_HANDLE; }

    // Record the terrain into the frame. `origin` is the world position of the
    // tile's minimum XZ corner (origin.y offsets all heights); `snowline_m` is
    // the world height where snow takes over. Declare AFTER the mesh pass —
    // the chunks render into its GBuffer attachments.
    void add_to_graph(rhi::RenderGraph& graph, const renderer::GBufferTargets& gbuffer,
                      VkExtent2D extent, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                      const glm::vec3& origin, float snowline_m);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::GraphicsPipeline          pipeline_;
    rhi::Sampler                   sampler_;
    rhi::DescriptorBuffer          descriptor_; // heightmap + sampler, persistent

    // Shared chunk index buffers, one per LOD (grid triangles + skirt ring).
    struct LodIndices {
        rhi::GpuBuffer buffer;
        std::uint32_t  count = 0;
    };
    std::vector<LodIndices> lods_;

    rhi::GpuImage  heightmap_;
    rhi::ImageView heightmap_view_;
    std::uint32_t  resolution_ = 0;
    float          tile_size_m_ = 0.0F;
    float          min_height_ = 0.0F;
    float          max_height_ = 0.0F;

    struct ChunkDraw {
        glm::vec4 chunk;  // origin xz, size, skirt depth
        glm::vec4 region; // uv offset, uv scale, metres/texel
        std::uint32_t lod = 0;
    };
    std::vector<ChunkDraw> frame_draws_; // rebuilt each add_to_graph, consumed same frame
};

// Offscreen self-test: generate a small tile, upload it, render it over a
// cleared GBuffer from a top-down camera, and verify the centre albedo texel
// is terrain-coloured (the geometry pipeline + VTF + splat actually ran).
[[nodiscard]] std::expected<void, core::Error>
run_terrain_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                           rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                           const std::string& cooked_shader_dir);

} // namespace engine::renderer
