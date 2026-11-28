#pragma once

// Per-tile terrain surface attributes for the layered material (ROADMAP
// R26.1): curvature and flow accumulation baked on the GPU from a tile's
// heightmap (terrain_attributes.slang) into an RGBA8 image the terrain pass
// samples — r = concavity * 0.5 + 0.5 (hollows > 0.5, crests < 0.5), g = flow
// wetness (0 dry .. 1 water collects). engine::renderer::
// terrain_attributes_reference (terrain_material_gpu.hpp) is its CPU mirror.
//
// schedule() creates the image and queues its bake; record() (inside a
// compute pass of the frame graph, before the terrain draws) runs every
// queued bake and leaves the images in SHADER_READ_ONLY_OPTIMAL. A bake's
// scratch buffers are freed a few frames later by tick().

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

class TerrainAttributeBaker {
public:
    [[nodiscard]] static std::expected<TerrainAttributeBaker, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    TerrainAttributeBaker() = default;
    TerrainAttributeBaker(TerrainAttributeBaker&&) noexcept = default;
    TerrainAttributeBaker& operator=(TerrainAttributeBaker&&) noexcept = default;
    TerrainAttributeBaker(const TerrainAttributeBaker&) = delete;
    TerrainAttributeBaker& operator=(const TerrainAttributeBaker&) = delete;

    // Largest tile resolution baked; larger tiles keep flat attributes.
    static constexpr std::uint32_t max_resolution = 2049;

    struct Target {
        rhi::GpuImage  image; // RGBA8, written by the bake (TRANSFER_SRC for tests)
        rhi::ImageView view;
    };
    // Creates a tile's attribute image and queues its bake from `heights`
    // (an R32F view in SHADER_READ_ONLY_OPTIMAL, resolution² texels,
    // `metres_per_texel` apart). The caller owns the image and must cancel()
    // it before destroying it while a bake is still queued.
    [[nodiscard]] std::expected<Target, core::Error>
    schedule(VkImageView heights, std::uint32_t resolution, float metres_per_texel,
             std::uint32_t sweeps);

    // Drops the queued bake of `image` (no-op when none is queued).
    void cancel(VkImage image);

    [[nodiscard]] bool pending() const { return !queued_.empty(); }

    // Records every queued bake into `cmd` (compute, outside rendering).
    void record(VkCommandBuffer cmd);

    // Frees the scratch of bakes recorded a few frames ago (once a frame).
    void tick();

    [[nodiscard]] bool valid() const { return device_ != nullptr; }

private:
    enum Kernel : std::uint8_t { weights, flow_init, flow_step, finalize, kernel_count };
    struct Bake {
        VkImage               image = VK_NULL_HANDLE;
        std::uint32_t         resolution = 0;
        float                 metres = 0.0F;
        std::uint32_t         sweeps = 0;
        rhi::GpuBuffer        flow;
        rhi::GpuBuffer        wsum;
        rhi::DescriptorBuffer set;
        int                   frames_left = 3;
    };

    const rhi::Device*             device_ = nullptr;
    rhi::GpuAllocator*             allocator_ = nullptr;
    rhi::DescriptorBufferFunctions fns_{};
    rhi::DescriptorSetLayout       layout_;
    std::vector<std::uint32_t>     slots_; // g_height, g_flow, g_wsum, g_out
    rhi::ManagedPipeline           pipelines_[kernel_count];
    std::vector<Bake>              queued_;
    std::vector<Bake>              recorded_;
};

} // namespace engine::renderer
