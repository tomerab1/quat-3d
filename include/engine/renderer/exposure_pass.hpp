#pragma once

// ExposurePass — wide-range histogram auto-exposure (R22.2.1; Phase 8.3).
//
// Builds a 256-bin log2-luminance histogram of the pre-exposed HDR image
// over 40 EV (photometry.hpp: a 10⁻⁴ lux night to a 10⁵ lux noon), meters
// the mean log luminance between two percentiles, eases the adapted value
// toward it in the log domain, and turns it into an exposure through the
// adaptive key (Krawczyk et al. 2005) clamped to an EV100 range. The tonemap
// and TAA passes read that exposure; the frame renderer reads it back to
// pre-expose the next frame. (Histogram metering: Opsenica 2019 and Unreal;
// pre-exposure: Lagarde & de Rousiers 2014.)
//
// The adapted luminance lives in an ExposureState shared by every
// frame-in-flight slot (audit A11): each frame advances the one value by that
// frame's dt, so the time constant is the configured one and consecutive
// frames never alternate between two independently adapting values.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Persistent auto-exposure state (host-visible; exposure_average.slang):
// [0] adapted luminance (HDR units, not pre-exposed; 0 until the first
// measured frame seeds it), [1] auto exposure before compensation,
// [2] adapted log2 luminance, [3] the key it maps to. One per view, shared by
// every slot's ExposurePass.
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
    static constexpr std::size_t k_floats = 4;

    // Current adapted average luminance (read from the mapped buffer).
    [[nodiscard]] float adapted_luminance() const;
    // Current auto exposure (HDR units, before compensation; 1 before the
    // first frame).
    [[nodiscard]] float exposure() const;
    [[nodiscard]] float key() const;

private:
    rhi::GpuBuffer  buffer_;
    VkDeviceAddress address_ = 0;
};

// How one frame meters and adapts (from ExposureSettings, plus the
// frame's pre-exposure).
struct ExposureMetering {
    float adaptation_tau = 1.1F;    // seconds
    float key            = 0.18F;
    bool  adaptive_key   = true;
    float min_ev100      = -14.0F;
    float max_ev100      = 20.0F;
    float low_percent    = 0.10F;
    float high_percent   = 0.90F;
    // Centre-weighted metering strength (photometry::metering_weight).
    float center_weight  = 0.75F;
    // Seed from this frame's metering instead of adapting (a camera cut).
    bool  reset          = false;
    // The multiplier the HDR image was pre-exposed with (> 0).
    float pre_exposure   = 1.0F;

    [[nodiscard]] static ExposureMetering from(const ExposureSettings& settings,
                                               float pre_exposure);
};

class ExposurePass {
public:
    // Default adaptation time constant (seconds): after t seconds of a constant
    // scene the remaining error is exp(-t / tau).
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

    // Update the shared adapted luminance and exposure from `hdr`,
    // advancing them by `dt` seconds (the real time since the previous
    // frame, whichever slot recorded it). Pass exposure_buffer_address() to
    // TonemapPass::add_to_graph.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent, float dt,
                 const ExposureMetering& metering = {});

    [[nodiscard]] VkDeviceAddress exposure_buffer_address() const { return state_->address(); }

    // Current adapted average luminance (the exposure buffer is host-visible).
    [[nodiscard]] float adapted_luminance() const { return state_->adapted_luminance(); }
    [[nodiscard]] float exposure() const { return state_->exposure(); }

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

} // namespace engine::renderer
