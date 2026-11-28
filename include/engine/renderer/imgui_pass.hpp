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
//
// Colour space (issue #92): ImGui's colours and its font antialiasing are
// authored for blending in gamma (sRGB-encoded) space. The pass therefore
// passes vertex colours through unmodified and blends on the target's stored
// bytes — never in linear space, which washes dark-on-light text out to grey:
//   * a UNORM target is drawn directly;
//   * an sRGB target with a UNORM view (RenderGraph::set_unorm_view; the
//     mutable-format swapchain, VK_KHR_swapchain_mutable_format) is drawn
//     through that view;
//   * any other sRGB target gets the UI drawn into a UNORM scratch image whose
//     bytes are then copied over it (overlay mode first copies the target's
//     bytes in) — bit-identical to the view path, one or two copies dearer.
// Textures are sampled the same way, without sRGB decode: the font atlas is
// UNORM, and the viewport image must be UNORM or carry a UNORM view, so the
// tonemapped scene's encoded bytes reach the screen exactly as in the game
// view.

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

struct ImDrawData;

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

class ImGuiPass {
public:
    // ImTextureID values understood by this renderer. The font atlas id is
    // assigned to the ImGui context at create(); the viewport id is what the
    // editor's Viewport panel passes to ImGui::Image — it resolves to the
    // offscreen scene image handed to add_to_graph.
    static constexpr std::uint64_t font_texture_id     = 1;
    static constexpr std::uint64_t viewport_texture_id = 2;

    // Builds the UI pipeline for targets of `output_format` (an sRGB format
    // or its UNORM twin; the pipeline renders the UNORM one) and uploads the
    // font atlas of the CURRENT ImGui context — one must exist before this
    // call.
    [[nodiscard]] static std::expected<ImGuiPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const rhi::TransferContext& transfer, const std::string& cooked_shader_dir,
           VkFormat output_format);

    ImGuiPass() = default;
    ImGuiPass(ImGuiPass&&) noexcept = default;
    ImGuiPass& operator=(ImGuiPass&&) noexcept = default;
    ImGuiPass(const ImGuiPass&) = delete;
    ImGuiPass& operator=(const ImGuiPass&) = delete;

    // Re-uploads the current ImGui context's font atlas (after the editor
    // rebuilt it for a new display scale). The caller guarantees the GPU no
    // longer uses this pass's previous atlas (device idle).
    [[nodiscard]] std::expected<void, core::Error>
    upload_font_atlas(const rhi::TransferContext& transfer);

    // Copies `draw_data` (vertices, indices, clipped draw commands) into this
    // pass's host-visible buffers and appends a graphics pass compositing the
    // UI over `target`. No-op when there is nothing to draw. The copy means
    // `draw_data` only needs to stay valid for this call, and each
    // frame-in-flight slot owns its own ImGuiPass instance.
    //
    // `viewport`, when valid, is the offscreen scene image the Viewport panel
    // samples (ImGui::Image with viewport_texture_id); the pass declares the
    // read so the graph orders/transitions it. It is sampled through its
    // UNORM view (stored bytes); an sRGB viewport without one is an error.
    // `clear` clears `target` first (editor mode owns the whole swapchain)
    // instead of compositing (LOAD). `target` must be of the create() format
    // family; see the header comment for how an sRGB target is reached.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle target, VkExtent2D extent,
                 const ImDrawData* draw_data, rhi::ResourceHandle viewport = {},
                 bool clear = false);

private:
    struct UiDrawCmd {
        VkRect2D      scissor{};
        std::uint32_t index_count = 0;
        std::uint32_t first_index = 0;
        std::int32_t  vertex_offset = 0;
        std::uint64_t texture = font_texture_id;
    };

    [[nodiscard]] std::expected<void, core::Error> ensure_buffers(VkDeviceSize vertex_bytes,
                                                                  VkDeviceSize index_bytes);

    const rhi::Device* device_    = nullptr; // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    VkFormat                       pipeline_format_ = VK_FORMAT_UNDEFINED; // UNORM
    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;

    rhi::GpuImage         font_image_;
    rhi::ImageView        font_view_;
    rhi::Sampler          font_sampler_;
    // Created lazily on first add_to_graph — a DescriptorBuffer references the
    // functions/layout members above, so it must be built after this pass has
    // stopped moving.
    rhi::DescriptorBuffer font_descriptor_;
    bool                  font_descriptor_ready_ = false;
    // Same, for the Viewport panel's scene image (rewritten every frame — the
    // resolved view changes when the panel is resized).
    rhi::DescriptorBuffer viewport_descriptor_;
    bool                  viewport_descriptor_ready_ = false;

    // Host-visible, persistently mapped; grown on demand, rewritten per frame.
    rhi::GpuBuffer vertex_buffer_;
    rhi::GpuBuffer index_buffer_;

    // Per-frame state captured by add_to_graph for the execute lambda.
    std::vector<UiDrawCmd> frame_cmds_;
    float                  frame_scale_[2]{1.0F, 1.0F};
    float                  frame_translate_[2]{-1.0F, -1.0F};
};

} // namespace engine::renderer
