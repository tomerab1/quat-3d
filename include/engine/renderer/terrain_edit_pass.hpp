#pragma once

// TerrainEditPass — terrain edit stamps applied on the GPU (ROADMAP G19.1).
//
// The GPU path of engine/terrain/edits.hpp: TerrainSynthPass runs it on every
// R10.2 tile it synthesises while the scene has edits, between the last
// erosion dispatch and the tile's upload, so the tile image, its paint image
// and the read-back heights all carry the edits. The CPU reference
// (terrain::apply_edits) stays authoritative for interactive edits (small
// regions, needed on the CPU anyway for physics), and the tests hold the two
// within a tolerance (assets/shaders/terrain_edit.slang explains why not bit
// for bit).
//
// prepare() builds a tile's resources: the packed stamps (host-visible), the
// generated-heights copy, a smooth scratch and the RGBA8 paint (storage
// buffers, the paint also a transfer source for its image). record() then
// applies them to a heights storage buffer in one command buffer: edit_init,
// then a pointwise dispatch per run of non-smooth stamps and two dispatches
// per smooth iteration, separated by compute barriers.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/terrain/edits.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

class TerrainEditPass {
public:
    [[nodiscard]] static std::expected<TerrainEditPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    TerrainEditPass();
    ~TerrainEditPass();
    TerrainEditPass(TerrainEditPass&&) noexcept;
    TerrainEditPass& operator=(TerrainEditPass&&) noexcept;
    TerrainEditPass(const TerrainEditPass&) = delete;
    TerrainEditPass& operator=(const TerrainEditPass&) = delete;

    [[nodiscard]] bool valid() const { return state_ != nullptr; }

    // One tile's edit resources. `heights` is the caller's storage buffer of
    // res² floats the edits apply to in place.
    struct Target {
        terrain::PackedStamps packed;
        glm::ivec2            tile{0, 0};
        std::uint32_t         res = 0;
        float                 tile_size_m = 0.0F;
        rhi::GpuBuffer        stamps, points, edges; // host-visible inputs
        rhi::GpuBuffer        base;    // float[res²]: the generated heights
        rhi::GpuBuffer        scratch; // float[res²]
        rhi::GpuBuffer        paint;   // uint[res²]: RGBA8
        rhi::DescriptorBuffer descriptor;
    };
    [[nodiscard]] std::expected<Target, core::Error>
    prepare(terrain::PackedStamps packed, glm::ivec2 tile, std::uint32_t res, float tile_size_m,
            VkDeviceAddress heights, VkDeviceSize heights_bytes) const;

    // Records the edits of `target` (compute stage; the caller synchronises
    // the heights buffer before and the outputs after).
    void record(VkCommandBuffer cmd, const Target& target) const;

private:
    struct State;
    std::unique_ptr<State> state_; // heap-held: descriptor buffers point at the layout
};

} // namespace engine::renderer
