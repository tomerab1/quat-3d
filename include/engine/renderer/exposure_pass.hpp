#pragma once

// ExposurePass — histogram auto-exposure (Phase 8.3).
//
// Builds a 256-bin log-luminance histogram of the HDR image, reduces it to an
// average luminance (excluding black), and eases an adapted-luminance value
// toward it over time (exponential). The tonemap pass reads that value to scale
// exposure = key / adapted-luminance. (Tardif / DICE automatic exposure.)
//
// The adapted luminance lives in an ExposureState shared by every
// frame-in-flight slot (audit A11): each frame advances the one value by that
// frame's dt, so the time constant is the configured one and consecutive
// frames never alternate between two independently adapting values.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Persistent auto-exposure state: the adapted average luminance (one float,
// host-visible; starts at 0 so the first measured frame seeds it directly).
// One per view, shared by every slot's ExposurePass.
class ExposureState {
    struct Key {
        explicit Key() = default;
    };

public:
    [[nodiscard]] static std::expected<std::shared_ptr<ExposureState>, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator);

    // Construct through create(); the key keeps the constructor private-only
    // while letting create() use std::make_shared.
    explicit ExposureState(Key /*key*/) {}

    ExposureState(const ExposureState&) = delete;
    ExposureState& operator=(const ExposureState&) = delete;

    [[nodiscard]] VkBuffer        buffer() const { return buffer_.handle(); }
    [[nodiscard]] VkDeviceAddress address() const { return address_; }
    // Current adapted average luminance (read from the mapped buffer).
    [[nodiscard]] float adapted_luminance() const;

private:
    rhi::GpuBuffer  buffer_;
    VkDeviceAddress address_ = 0;
};

class ExposurePass {
public:
    // Adaptation time constant (seconds): after t seconds of a constant scene the
    // remaining error is exp(-t / k_adaptation_tau).
    static constexpr float k_adaptation_tau = 1.1F;

    // `state` is shared by every frame-in-flight slot's pass; null creates a
    // private one (single-slot use, tests).
    [[nodiscard]] static std::expected<ExposurePass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::shared_ptr<ExposureState> state = nullptr);

    ExposurePass() = default;
    ExposurePass(ExposurePass&&) noexcept = default;
    ExposurePass& operator=(ExposurePass&&) noexcept = default;
    ExposurePass(const ExposurePass&) = delete;
    ExposurePass& operator=(const ExposurePass&) = delete;

    // Update the shared adapted luminance from `hdr`, advancing it by `dt`
    // seconds (the real time since the previous frame, whichever slot recorded
    // it). Pass exposure_buffer_address() to TonemapPass::add_to_graph.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent, float dt);

    [[nodiscard]] VkDeviceAddress exposure_buffer_address() const { return state_->address(); }

    // Current adapted average luminance (the exposure buffer is host-visible).
    [[nodiscard]] float adapted_luminance() const { return state_->adapted_luminance(); }

    [[nodiscard]] ExposureState& state() { return *state_; }

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       histogram_layout_; // sampled image, storage buffer
    rhi::DescriptorSetLayout       average_layout_;   // storage buffer, storage buffer
    rhi::ManagedPipeline           histogram_pipeline_;
    rhi::ManagedPipeline           average_pipeline_;

    rhi::GpuBuffer  histogram_buffer_;  // 256 uint bins (per slot, cleared each frame)
    VkDeviceAddress histogram_address_ = 0;
    std::shared_ptr<ExposureState> state_; // adapted luminance (shared, persists)

    rhi::DescriptorBuffer histogram_descriptor_;
    rhi::DescriptorBuffer average_descriptor_;
};

// Self-test: a uniform dim HDR image adapts to ~its luminance; a uniform bright
// image then pushes the adapted luminance up. Verifies the histogram measures
// luminance and the temporal adaptation tracks it.
[[nodiscard]] std::expected<void, core::Error>
run_exposure_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
