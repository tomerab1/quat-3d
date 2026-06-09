#pragma once

// BloomPass — HDR bloom (Phase 8.2).
//
// A soft-knee bright-pass threshold followed by a progressive downsample
// (13-tap) / upsample (9-tap tent) blur chain, composited additively back into
// the HDR colour buffer before tonemap. This gives highlights (the sun, glass
// speculars, emissive surfaces) a physically-plausible glow.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

struct BloomParams {
    float threshold = 1.0F; // luminance above which highlights bloom
    float knee = 0.6F;      // soft-knee width around the threshold
    float intensity = 0.06F; // bloom contribution added to the HDR buffer
    float radius = 1.0F;    // upsample tent radius (blur spread)
};

class BloomPass {
public:
    [[nodiscard]] static std::expected<BloomPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    BloomPass() = default;
    BloomPass(BloomPass&&) noexcept = default;
    BloomPass& operator=(BloomPass&&) noexcept = default;
    BloomPass(const BloomPass&) = delete;
    BloomPass& operator=(const BloomPass&) = delete;

    // Blur the bright parts of `hdr` and add them back into it (read-modify-write)
    // before tonemap. Declares all passes into `graph`; must be compiled +
    // executed before the next call (owns per-frame descriptor buffers).
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent,
                 const BloomParams& params);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       down_layout_;  // sampled, sampler, storage
    rhi::DescriptorSetLayout       up_layout_;     // sampled, sampler, sampled, storage
    rhi::ComputePipeline           down_pipeline_;
    rhi::ComputePipeline           up_pipeline_;
    rhi::ComputePipeline           composite_pipeline_;
    rhi::Sampler                   sampler_;

    std::vector<rhi::DescriptorBuffer> frame_descriptors_; // rebuilt each add_to_graph
};

// Self-test: a single bright block on a black HDR image must, after bloom, glow
// into the surrounding (previously black) pixels while the block stays bright.
[[nodiscard]] std::expected<void, core::Error>
run_bloom_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
