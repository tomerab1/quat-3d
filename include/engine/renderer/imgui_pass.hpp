#pragma once

// ImGuiPass — editor UI overlay (Phase 9, Slice 9.1).
//
// Renders ImGui draw data over the final swapchain image as the last graph
// pass. The engine's descriptor model is VK_EXT_descriptor_buffer, which may
// not be mixed with classic descriptor sets in one command buffer — so this is
// a from-scratch ImGui renderer on the engine RHI (pipeline + descriptor
// buffer + per-frame vertex upload), NOT the stock imgui_impl_vulkan backend
// (which allocates descriptor sets from pools). Only the SDL3 platform backend
// (input/windowing, no Vulkan) is reused, in src/editor.
//
// ImGui types stay out of this header (forward-declared ImDrawData) per the
// project rule: ImGui headers only in src/editor/ and src/renderer/imgui_pass.cpp.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

struct ImDrawData;

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

class ImGuiPass {
public:
    // Builds the UI pipeline (targeting `output_format`) and uploads the font
    // atlas of the CURRENT ImGui context — one must exist before this call.
    [[nodiscard]] static std::expected<ImGuiPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const rhi::TransferContext& transfer, const std::string& cooked_shader_dir,
           VkFormat output_format);

    ImGuiPass() = default;
    ImGuiPass(ImGuiPass&&) noexcept = default;
    ImGuiPass& operator=(ImGuiPass&&) noexcept = default;
    ImGuiPass(const ImGuiPass&) = delete;
    ImGuiPass& operator=(const ImGuiPass&) = delete;

    // Copies `draw_data` (vertices, indices, clipped draw commands) into this
    // pass's host-visible buffers and appends a graphics pass compositing the
    // UI over `target` (loadOp LOAD). No-op when there is nothing to draw.
    // The copy means `draw_data` only needs to stay valid for this call, and
    // each frame-in-flight slot owns its own ImGuiPass instance.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle target, VkExtent2D extent,
                 const ImDrawData* draw_data);

private:
    struct UiDrawCmd {
        VkRect2D      scissor{};
        std::uint32_t index_count = 0;
        std::uint32_t first_index = 0;
        std::int32_t  vertex_offset = 0;
    };

    [[nodiscard]] std::expected<void, core::Error> ensure_buffers(VkDeviceSize vertex_bytes,
                                                                  VkDeviceSize index_bytes);

    const rhi::Device* device_    = nullptr; // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::GraphicsPipeline          pipeline_;

    rhi::GpuImage         font_image_;
    rhi::ImageView        font_view_;
    rhi::Sampler          font_sampler_;
    // Created lazily on first add_to_graph — a DescriptorBuffer references the
    // functions/layout members above, so it must be built after this pass has
    // stopped moving.
    rhi::DescriptorBuffer font_descriptor_;
    bool                  font_descriptor_ready_ = false;

    // Host-visible, persistently mapped; grown on demand, rewritten per frame.
    rhi::GpuBuffer vertex_buffer_;
    rhi::GpuBuffer index_buffer_;

    // Per-frame state captured by add_to_graph for the execute lambda.
    std::vector<UiDrawCmd> frame_cmds_;
    float                  frame_scale_[2]{1.0F, 1.0F};
    float                  frame_translate_[2]{-1.0F, -1.0F};
};

// Offscreen self-test: builds a private ImGui context, draws one solid window,
// renders it over a cleared target, and verifies covered/uncovered pixels.
[[nodiscard]] std::expected<void, core::Error>
run_imgui_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                         const std::string& cooked_shader_dir);

} // namespace engine::renderer
