#pragma once

// Upscaler — one interface for temporal anti-aliasing and upscaling backends
// (ROADMAP R11; design and the FSR plan in docs/upscaling.md).
//
// Every backend consumes the same inputs, produced by the frame graph at the
// render extent: pre-tonemap linear HDR colour, depth, the velocity target
// (velocity.hpp convention), the frame's jitter (jitter.hpp), the exposure,
// and an optional reactive mask. It returns HDR colour at the display extent;
// post-processing (bloom, exposure, tonemap) and the UI run after it at
// display resolution. Modes pick the render scale; native_aa renders at
// display resolution and only anti-aliases.
//
// Backends (UpscalerBackend, upscaler_settings.hpp): taau (TaaUpscaler,
// taa_pass.hpp — the engine's own temporal upsampler) and fsr2 (AMD
// FidelityFX FSR 2, not built yet: create_upscaler reports it unavailable and
// callers fall back to taau).

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/jitter.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/renderer/upscaler_settings.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

// One frame's inputs. Images are graph resources the upscale pass reads as
// sampled images in a compute shader.
struct UpscalerInputs {
    rhi::ResourceHandle color;    // linear HDR before tonemapping, render extent
    rhi::ResourceHandle depth;    // GBuffer depth ([0, 1], 1 = far), render extent
    rhi::ResourceHandle velocity; // velocity_format, render extent
    // Optional (invalid = none): R8 UNORM at render extent, 1 = the pixel's
    // history is unreliable (particles, alpha-blended surfaces).
    rhi::ResourceHandle reactive{};
    // Device address of the adapted average luminance (ExposureState; one
    // float, last written by the previous frame's exposure pass). 0 = an
    // exposure of 1. Backends weight their history in exposed, tonemapped
    // space so HDR highlights do not dominate the resolve.
    VkDeviceAddress exposure = 0;
    VkExtent2D      render_extent{0, 0};
    VkExtent2D      display_extent{0, 0};
    ViewJitter      jitter{};
    float           sharpness = 0.0F;
    // History weight a pixel converges to with a sample on it every frame
    // (A8's taa.feedback, [0, 0.98]); TAAU caps a pixel's accumulated sample
    // weight at 1 / (1 - feedback).
    float           feedback = 0.9F;
    bool            reset = false; // camera cut: ignore the history this frame
    // Debug (issue #45): device address of a uint32 the resolve increments for
    // every non-finite colour sample, reprojection or history value it
    // rejects (NonFiniteCounters::address, NonFiniteSource::taa_history; bound
    // as a storage-buffer descriptor); 0 = off.
    VkDeviceAddress non_finite_counter = 0;
};

// Applies A8's TAA settings to one frame's inputs: off resolves every frame
// without history (the current frame passes through at native scale), and
// `feedback` becomes the resolve's history weight.
void apply_taa_settings(UpscalerInputs& in, const TaaSettings& taa);

class Upscaler {
public:
    virtual ~Upscaler() = default;

    [[nodiscard]] virtual UpscalerBackend backend() const = 0;

    // Declares the upscale for frame-in-flight `slot` (whose previous frame
    // has finished) and returns the display-extent output: RGBA16F with
    // STORAGE | SAMPLED | TRANSFER_SRC usage, written by the graph.
    [[nodiscard]] virtual std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot, const UpscalerInputs& in) = 0;

    // Forget the accumulated history (scene load, teleport).
    virtual void reset_history() = 0;
};

// Creates the requested backend; an error names why it is unavailable.
[[nodiscard]] std::expected<std::unique_ptr<Upscaler>, core::Error>
create_upscaler(UpscalerBackend backend, const rhi::Device& device,
                rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
                const std::string& cooked_shader_dir, std::uint32_t frames_in_flight);

} // namespace engine::renderer
