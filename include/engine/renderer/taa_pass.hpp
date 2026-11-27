#pragma once

// TaaPass / TaaUpscaler — temporal anti-aliasing and upsampling (TAAU), the
// engine's own Upscaler backend (ROADMAP R11; Phase 8.1 TAA, history sharing
// fixed in S1 audit A2).
//
// Resolves the jittered, render-extent HDR frame against a history at the
// display extent (taa.slang): the history is reprojected with the velocity
// target (velocity.hpp) at the closest-depth sample of each pixel's
// neighbourhood and clipped toward the current frame's neighbourhood
// statistics. At native resolution this is TAA; below it, each display pixel
// accumulates the jittered render samples that land on it (TAAU). The jitter
// comes from jitter.hpp, so render samples, velocity and the resolve agree.
//
// The history lives in a TaaHistory shared by every frame-in-flight slot. Each
// slot owns its own TaaPass (per-slot descriptor buffer); all of them resolve
// into the one history pair, which ping-pongs on the history's own frame
// counter, so every frame reads the image the previous frame wrote — whichever
// slot recorded it. TaaUpscaler bundles the history with one pass per slot
// behind the Upscaler interface.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/upscaler.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Temporal history shared by every frame-in-flight slot of one view (audit A2).
// Owns the ping-pong pair of display-extent RGBA32F images (rgb colour, a the
// accumulated sample weight), the frame counter that picks the ping-pong
// slot, and history validity. Recreated (and invalidated) when the display
// extent changes; replaced images stay alive until every frame that may still
// use them has retired.
//
// 32-bit floats, because the running average blends a far-away sample in with
// weights well below 1/1000, under half-float rounding: an RGBA16F history
// stalls and drifts (the 50% upscale test's error rose from 0.0017 to 0.0047).
// RGBA32F storage images are mandatory in Vulkan, so it works everywhere.
//
// Contract: the caller waits on a slot's fence before recording that slot's
// frame (the standard frame loop), records one resolve per frame, and compiles
// and executes every graph the TAA pass was added to.
class TaaHistory {
public:
    static constexpr VkFormat k_format = VK_FORMAT_R32G32B32A32_SFLOAT;

    TaaHistory(const rhi::Device& device, rhi::GpuAllocator& allocator,
               std::uint32_t frames_in_flight);

    TaaHistory(const TaaHistory&) = delete;
    TaaHistory& operator=(const TaaHistory&) = delete;

    // Drop the accumulated history (camera cut, scene load); the next resolve
    // shows the current frame unblended.
    void invalidate() { valid_ = false; }

    // True when the next resolve will blend with the history.
    [[nodiscard]] bool valid() const { return valid_; }
    // Number of resolves recorded so far (selects the ping-pong slot).
    [[nodiscard]] std::uint64_t frame_count() const { return frame_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }

private:
    friend class TaaPass;

    // One history image plus what the last recorded frame left it in. The next
    // frame imports it with exactly this state, so the render graph's first
    // barrier on it waits for the previous frame's access (cross-frame hazard:
    // both frames-in-flight run on the same queue and may overlap).
    struct Image {
        rhi::GpuImage         image;
        rhi::ImageView        view;
        VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2        access = 0;
    };
    struct Retired {
        std::array<Image, 2> images;
        std::uint64_t        last_frame = 0; // last resolve that used them
    };

    // (Re)create the pair at `extent` if needed; doing so invalidates history.
    [[nodiscard]] std::expected<void, core::Error> ensure(VkExtent2D extent);
    // Destroy retired images that no in-flight frame can still be using.
    void collect_retired();

    const rhi::Device*   device_           = nullptr;
    rhi::GpuAllocator*   allocator_        = nullptr;
    std::uint32_t        frames_in_flight_ = 2;
    std::array<Image, 2> images_;
    std::vector<Retired> retired_;
    VkExtent2D           extent_{0, 0};
    std::uint64_t        frame_ = 0;
    bool                 valid_ = false;
};

class TaaPass {
public:
    // The resolved image add_to_graph returns (Upscaler::add_to_graph contract).
    static constexpr VkFormat k_output_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // `history` is shared by every slot's pass; null creates a private one
    // (single-slot use, tests).
    [[nodiscard]] static std::expected<TaaPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::shared_ptr<TaaHistory> history = nullptr);

    TaaPass() = default;
    TaaPass(TaaPass&&) noexcept = default;
    TaaPass& operator=(TaaPass&&) noexcept = default;
    TaaPass(const TaaPass&) = delete;
    TaaPass& operator=(const TaaPass&) = delete;

    // Resolves `in` against the shared history and returns the display-extent
    // result (see Upscaler::add_to_graph). Advances the history. The reactive
    // mask is not consumed yet (transparents and particles do not write one).
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const UpscalerInputs& in);

    [[nodiscard]] TaaHistory& history() { return *history_; }

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    rhi::Sampler                   sampler_;
    std::vector<std::uint32_t>     slots_; // reflected binding of each resource
    rhi::GpuBuffer                 unit_exposure_; // bound when no exposure is given
    VkDeviceAddress                unit_exposure_address_ = 0;

    std::shared_ptr<TaaHistory> history_;
    rhi::DescriptorBuffer       frame_descriptor_;
};

// The TAAU backend of the Upscaler interface: one TaaPass per frame slot
// sharing one history.
class TaaUpscaler final : public Upscaler {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<TaaUpscaler>, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::uint32_t frames_in_flight);

    [[nodiscard]] UpscalerBackend backend() const override { return UpscalerBackend::taau; }
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot, const UpscalerInputs& in) override;
    void reset_history() override { history_->invalidate(); }

    [[nodiscard]] TaaHistory& history() { return *history_; }

private:
    std::shared_ptr<TaaHistory> history_;
    std::vector<TaaPass>        passes_;
};

} // namespace engine::renderer
