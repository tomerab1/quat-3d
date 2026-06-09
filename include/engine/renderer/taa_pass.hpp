#pragma once

// TaaPass — temporal anti-aliasing (Phase 8.1).
//
// Resolves the camera-jittered current frame against a reprojected history
// (taa.slang) into a ping-ponged history image, then copies the result to the
// swapchain. The caller jitters the projection per frame (Halton) and supplies
// the current inverse view-projection + the previous frame's view-projection for
// reprojection.

#include <cstdint>
#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Sub-pixel camera jitter (NDC units) for frame `index`, from the Halton(2,3)
// sequence. Add to projection[2][0]/[2][1] (the w-multiplied clip translation).
[[nodiscard]] glm::vec2 taa_jitter(std::uint64_t index, std::uint32_t width, std::uint32_t height);

class TaaPass {
public:
    [[nodiscard]] static std::expected<TaaPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, VkFormat ldr_format);

    TaaPass() = default;
    TaaPass(TaaPass&&) noexcept = default;
    TaaPass& operator=(TaaPass&&) noexcept = default;
    TaaPass(const TaaPass&) = delete;
    TaaPass& operator=(const TaaPass&) = delete;

    // Resolve `current` (jittered) against history using `depth` for reprojection
    // and copy the result to `swapchain`. `frame_index` selects the ping-pong
    // history slot; `history_valid` is false on the first frame / after a resize.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle current, rhi::ResourceHandle depth,
                 rhi::ResourceHandle swapchain, VkExtent2D extent,
                 const glm::mat4& cur_inv_view_proj, const glm::mat4& prev_view_proj,
                 std::uint64_t frame_index, bool history_valid);

private:
    [[nodiscard]] std::expected<void, core::Error> ensure_history(VkExtent2D extent);

    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;
    VkFormat           format_    = VK_FORMAT_UNDEFINED;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ComputePipeline           pipeline_;
    rhi::Sampler                   sampler_;

    rhi::GpuImage  history_[2];
    rhi::ImageView history_view_[2];
    VkExtent2D     history_extent_{0, 0};

    rhi::DescriptorBuffer frame_descriptor_;
};

// Self-test: a uniform frame resolves to itself (history clamped to the current
// neighbourhood). After history fills with value A, switching to a uniform value
// B must resolve to B — proving the neighbourhood clamp rejects stale history
// (no ghosting on flat regions).
[[nodiscard]] std::expected<void, core::Error>
run_taa_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
