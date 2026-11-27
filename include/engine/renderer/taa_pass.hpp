#pragma once

// TaaPass — temporal anti-aliasing (Phase 8.1; history sharing and unjittered
// reprojection fixed in S1, audit A2 + A14).
//
// Resolves the camera-jittered current frame against a reprojected history
// (taa.slang) and copies the result into the target (swapchain or editor
// viewport image). The caller jitters the projection per frame with
// taa_jitter() and passes the UNjittered view-projection: reprojection works
// between unjittered matrices, so a static camera reprojects every pixel onto
// itself and the history is never resampled at a sub-pixel offset.
//
// The history lives in a TaaHistory shared by every frame-in-flight slot. Each
// slot owns its own TaaPass (per-slot descriptor buffer); all of them resolve
// into the one history pair, which ping-pongs on the history's own frame
// counter, so every frame reads the image the previous frame wrote — whichever
// slot recorded it.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
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

// Sub-pixel camera jitter (NDC units) for frame `index`, from the Halton(2,3)
// sequence. Add to projection[2][0]/[2][1] (the w-multiplied clip translation).
[[nodiscard]] glm::vec2 taa_jitter(std::uint64_t index, std::uint32_t width, std::uint32_t height);

// Temporal history shared by every frame-in-flight slot of one view (audit A2).
// Owns the ping-pong pair of RGBA16F images (storage-capable everywhere, and
// float precision so accumulation never stalls on 8-bit rounding), the frame
// counter that picks the ping-pong slot, the previous frame's unjittered
// view-projection, and history validity. Recreated (and invalidated) when the
// resolve extent changes; replaced images stay alive until every frame that
// may still use them has retired.
//
// Contract: the caller waits on a slot's fence before recording that slot's
// frame (the standard frame loop), records one resolve per frame, and compiles
// and executes every graph the TAA pass was added to.
class TaaHistory {
public:
    static constexpr VkFormat k_format = VK_FORMAT_R16G16B16A16_SFLOAT;

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
    glm::mat4            prev_view_proj_{1.0F};
    std::uint64_t        frame_ = 0;
    bool                 valid_ = false;
};

class TaaPass {
public:
    // `ldr_format` is the format of the image being resolved and of the target
    // the result is copied into: R8G8B8A8 / A8B8G8R8 / B8G8R8A8 (UNORM or
    // SRGB). `history` is shared by every slot's pass; null creates a private
    // one (single-slot use, tests).
    [[nodiscard]] static std::expected<TaaPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, VkFormat ldr_format,
           std::shared_ptr<TaaHistory> history = nullptr);

    TaaPass() = default;
    TaaPass(TaaPass&&) noexcept = default;
    TaaPass& operator=(TaaPass&&) noexcept = default;
    TaaPass(const TaaPass&) = delete;
    TaaPass& operator=(const TaaPass&) = delete;

    // Resolve `current` (rendered with the jittered projection) against the
    // shared history, using `depth` for reprojection, and copy the result to
    // `target`. `view_proj` is this frame's UNjittered view-projection; the
    // history remembers it for the next frame. Advances the history.
    // `settings.feedback` is the history weight; with `settings.enabled` off the
    // resolve passes the current frame through (the caller stops jittering)
    // while still recording it as the next frame's history.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle current, rhi::ResourceHandle depth,
                 rhi::ResourceHandle target, VkExtent2D extent, const glm::mat4& view_proj,
                 const TaaSettings& settings = {});

    [[nodiscard]] TaaHistory& history() { return *history_; }

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;
    bool               swap_rb_   = false; // BGRA target: store the output swizzled

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    rhi::Sampler                   sampler_;

    std::shared_ptr<TaaHistory> history_;
    rhi::DescriptorBuffer       frame_descriptor_;
};

} // namespace engine::renderer
