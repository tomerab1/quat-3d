#pragma once

// BloomPass — HDR bloom (Phase 8.2).
//
// A soft-knee bright-pass threshold followed by a progressive downsample
// (13-tap) / upsample (9-tap tent) blur chain, composited additively back into
// the HDR colour buffer before tonemap. This gives highlights (the sun, glass
// speculars, emissive surfaces) a physically-plausible glow.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
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

    // Debug: device address of a uint32 the prefilter increments for every
    // non-finite texel group it drops (NonFiniteCounters::address, bound as a
    // storage-buffer descriptor); 0 turns it off.
    void set_non_finite_counter(VkDeviceAddress counter) { non_finite_counter_ = counter; }

    // Descriptor resources per shader (bloom_down, bloom_up, bloom_composite).
    static constexpr std::size_t k_down_resources      = 4;
    static constexpr std::size_t k_up_resources        = 4;
    static constexpr std::size_t k_composite_resources = 3;

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    // Layouts and binding slots reflected from each shader (bloom_pass.cpp
    // lists the resources by name).
    rhi::DescriptorSetLayout       down_layout_;
    rhi::DescriptorSetLayout       up_layout_;
    rhi::DescriptorSetLayout       composite_layout_;
    std::array<std::uint32_t, k_down_resources>      down_slots_{};
    std::array<std::uint32_t, k_up_resources>        up_slots_{};
    std::array<std::uint32_t, k_composite_resources> composite_slots_{};
    rhi::ManagedPipeline           down_pipeline_;
    rhi::ManagedPipeline           up_pipeline_;
    rhi::ManagedPipeline           composite_pipeline_;
    rhi::Sampler                   sampler_;

    std::vector<rhi::DescriptorBuffer> frame_descriptors_; // rebuilt each add_to_graph
    VkDeviceAddress                    non_finite_counter_ = 0;
    rhi::GpuBuffer                     unused_counter_; // bound when counting is off
    VkDeviceAddress                    unused_counter_address_ = 0;
};

} // namespace engine::renderer
