#include "engine/renderer/imgui_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <imgui.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// UI descriptor set 0: the bound texture (font atlas for now) + its sampler.
[[nodiscard]] std::array<rhi::DescriptorBinding, 2> ui_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
    }};
}

// Push constants: framebuffer-pixel -> clip-space mapping (imgui.slang).
struct UiPushConstants {
    float scale[2];
    float translate[2];
};
static_assert(sizeof(UiPushConstants) == 16, "must match imgui.slang PushConstants");

} // namespace

std::expected<ImGuiPass, core::Error>
ImGuiPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                  const std::string& cooked_shader_dir, VkFormat output_format) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return fail("imgui pass: no ImGui context — create the EditorLayer first");
    }

    ImGuiPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("imgui pass: descriptor buffer functions unavailable");
    }

    const auto bindings = ui_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/imgui.spv");
    if (!shader) return std::unexpected(shader.error());

    // Vertex input mirrors ImDrawVert (pos2 float, uv2 float, RGBA8 colour).
    const VkVertexInputBindingDescription vbind{0, sizeof(ImDrawVert),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 3> vattrs{{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)},
    }};
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(UiPushConstants)};
    const VkDescriptorSetLayout set_layout = out.layout_.handle();

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = {&output_format, 1};
    info.alpha_blend = true; // UI composites over the scene
    info.cull_mode = VK_CULL_MODE_NONE;
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};

    auto pipeline = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    // Font atlas: built once up front (the pre-1.92 "legacy" path — the engine
    // renderer does not advertise ImGuiBackendFlags_RendererHasTextures, so
    // ImGui keeps the atlas static after this build).
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return fail("imgui pass: font atlas build failed");
    }

    const VkExtent2D font_extent{static_cast<std::uint32_t>(width),
                                 static_cast<std::uint32_t>(height)};
    const VkDeviceSize font_bytes =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    auto image = rhi::upload_device_image(allocator, transfer, pixels, font_bytes, font_extent,
                                          VK_FORMAT_R8G8B8A8_UNORM);
    if (!image) return std::unexpected(image.error());
    out.font_image_ = std::move(*image);

    auto view = rhi::create_image_view(device.handle(), out.font_image_.handle(),
                                       VK_FORMAT_R8G8B8A8_UNORM);
    if (!view) return std::unexpected(view.error());
    out.font_view_ = std::move(*view);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.font_sampler_ = std::move(*sampler);

    // The font DescriptorBuffer is built lazily on first add_to_graph: a
    // DescriptorBuffer keeps pointers to the functions/layout it was created
    // with, and `out` is still going to move — creating it here would dangle.
    return out;
}

std::expected<void, core::Error> ImGuiPass::ensure_buffers(VkDeviceSize vertex_bytes,
                                                           VkDeviceSize index_bytes) {
    const auto grow = [&](rhi::GpuBuffer& buffer, VkDeviceSize needed,
                          VkBufferUsageFlags usage) -> std::expected<void, core::Error> {
        if (buffer.handle() != VK_NULL_HANDLE && buffer.size() >= needed) return {};
        // Headroom so steady UI growth does not reallocate every frame.
        const VkDeviceSize capacity = std::max<VkDeviceSize>(needed * 2, 16 * 1024);
        auto grown = allocator_->create_buffer(
            capacity, usage, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!grown) return std::unexpected(grown.error());
        buffer = std::move(*grown);
        return {};
    };
    if (auto r = grow(vertex_buffer_, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); !r) {
        return r;
    }
    return grow(index_buffer_, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

std::expected<void, core::Error> ImGuiPass::add_to_graph(rhi::RenderGraph& graph,
                                                         rhi::ResourceHandle target,
                                                         VkExtent2D extent,
                                                         const ImDrawData* draw_data) {
    frame_cmds_.clear();
    if (draw_data == nullptr || draw_data->TotalVtxCount <= 0 || draw_data->CmdListsCount <= 0) {
        return {};
    }
    static_assert(sizeof(ImDrawIdx) == 2, "pipeline binds VK_INDEX_TYPE_UINT16");

    // Deferred from create(): now `this` is at its final address, so the
    // descriptor buffer's internal references to db_fns_/layout_ stay valid.
    if (!font_descriptor_ready_) {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
        if (!db) return std::unexpected(db.error());
        font_descriptor_ = std::move(*db);
        font_descriptor_.write_sampled_image(0, font_view_.handle(),
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        font_descriptor_.write_sampler(1, font_sampler_.handle());
        font_descriptor_ready_ = true;
    }

    const VkDeviceSize vertex_bytes =
        static_cast<VkDeviceSize>(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
    const VkDeviceSize index_bytes =
        static_cast<VkDeviceSize>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);
    if (auto r = ensure_buffers(vertex_bytes, index_bytes); !r) return r;

    // Copy geometry into the mapped buffers and flatten the draw commands into
    // engine-side records (draw_data is owned by ImGui and rebuilt next frame;
    // the execute lambda below runs later, so it must not reference it).
    auto* vtx_dst = static_cast<ImDrawVert*>(vertex_buffer_.mapped());
    auto* idx_dst = static_cast<ImDrawIdx*>(index_buffer_.mapped());
    const ImVec2 clip_off = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale;
    std::int32_t  global_vtx = 0;
    std::uint32_t global_idx = 0;
    for (int li = 0; li < draw_data->CmdListsCount; ++li) {
        const ImDrawList* list = draw_data->CmdLists[li];
        std::memcpy(vtx_dst + global_vtx, list->VtxBuffer.Data,
                    static_cast<std::size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(idx_dst + global_idx, list->IdxBuffer.Data,
                    static_cast<std::size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));

        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) continue; // none used by the editor
            // Clip rect -> framebuffer scissor, clamped to the target.
            const float x0 = (cmd.ClipRect.x - clip_off.x) * clip_scale.x;
            const float y0 = (cmd.ClipRect.y - clip_off.y) * clip_scale.y;
            const float x1 = (cmd.ClipRect.z - clip_off.x) * clip_scale.x;
            const float y1 = (cmd.ClipRect.w - clip_off.y) * clip_scale.y;
            const std::int32_t sx0 = std::clamp(static_cast<std::int32_t>(x0), 0,
                                                static_cast<std::int32_t>(extent.width));
            const std::int32_t sy0 = std::clamp(static_cast<std::int32_t>(y0), 0,
                                                static_cast<std::int32_t>(extent.height));
            const std::int32_t sx1 = std::clamp(static_cast<std::int32_t>(x1), 0,
                                                static_cast<std::int32_t>(extent.width));
            const std::int32_t sy1 = std::clamp(static_cast<std::int32_t>(y1), 0,
                                                static_cast<std::int32_t>(extent.height));
            if (sx1 <= sx0 || sy1 <= sy0 || cmd.ElemCount == 0) continue;

            UiDrawCmd out{};
            out.scissor = {{sx0, sy0},
                           {static_cast<std::uint32_t>(sx1 - sx0),
                            static_cast<std::uint32_t>(sy1 - sy0)}};
            out.index_count = cmd.ElemCount;
            out.first_index = global_idx + cmd.IdxOffset;
            out.vertex_offset = global_vtx + static_cast<std::int32_t>(cmd.VtxOffset);
            frame_cmds_.push_back(out);
        }
        global_vtx += list->VtxBuffer.Size;
        global_idx += static_cast<std::uint32_t>(list->IdxBuffer.Size);
    }
    if (frame_cmds_.empty()) return {};

    frame_scale_[0] = 2.0F / draw_data->DisplaySize.x;
    frame_scale_[1] = 2.0F / draw_data->DisplaySize.y;
    frame_translate_[0] = -1.0F - draw_data->DisplayPos.x * frame_scale_[0];
    frame_translate_[1] = -1.0F - draw_data->DisplayPos.y * frame_scale_[1];

    auto pass = graph.add_pass("imgui", rhi::PassType::graphics);
    pass.writes(target, rhi::ResourceUsage::color_attachment)
        .execute([this, target, extent](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const rhi::ResourceBinding t = ctx.resolve(target);

            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = t.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // composite over the scene
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
            font_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0);

            UiPushConstants push{};
            push.scale[0] = frame_scale_[0];
            push.scale[1] = frame_scale_[1];
            push.translate[0] = frame_translate_[0];
            push.translate[1] = frame_translate_[1];
            vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);

            const VkBuffer vb = vertex_buffer_.handle();
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT16);

            for (const UiDrawCmd& dc : frame_cmds_) {
                vkCmdSetScissor(cmd, 0, 1, &dc.scissor);
                vkCmdDrawIndexed(cmd, dc.index_count, 1, dc.first_index, dc.vertex_offset, 0);
            }
            vkCmdEndRendering(cmd);
        });
    return {};
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

template <typename Record>
[[nodiscard]] std::expected<void, core::Error>
run_graphics_commands(const rhi::Device& device, Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return fail("imgui self-test: vkCreateCommandPool failed");
    }

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    const VkResult r = vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(device.graphics_queue());
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return r == VK_SUCCESS ? std::expected<void, core::Error>{}
                           : fail("imgui self-test: vkQueueSubmit2 failed");
}

} // namespace

std::expected<void, core::Error>
run_imgui_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                         const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{64, 64};
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    // Private context so the test never disturbs the editor's.
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    const auto restore = [&]() {
        ImGui::DestroyContext(ctx);
        ImGui::SetCurrentContext(previous);
    };

    auto pass = ImGuiPass::create(device, allocator, cache, transfer, cooked_shader_dir, format);
    if (!pass) {
        restore();
        return std::unexpected(pass.error());
    }

    // One borderless solid-red window covering the upper-left quadrant.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(32, 32));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0F, 0.0F, 0.0F, 1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::Begin("selftest", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Render();

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) {
        restore();
        return std::unexpected(readback.error());
    }

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle target = graph.create_transient_image(
        "ui_target", format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // Clear pass first: the UI pass uses loadOp LOAD, so the transient needs
    // defined contents underneath it.
    auto clear = graph.add_pass("clear", rhi::PassType::graphics);
    clear.writes(target, rhi::ResourceUsage::color_attachment)
        .execute([target, extent](rhi::PassContext& pass_ctx) {
            const rhi::ResourceBinding t = pass_ctx.resolve(target);
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = t.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(pass_ctx.cmd(), &rendering);
            vkCmdEndRendering(pass_ctx.cmd());
        });

    if (auto r = pass->add_to_graph(graph, target, extent, ImGui::GetDrawData()); !r) {
        restore();
        return std::unexpected(r.error());
    }
    if (auto compiled = graph.compile(); !compiled) {
        restore();
        return std::unexpected(compiled.error());
    }

    const VkImage target_image = graph.binding(target).image;
    const VkBuffer rb = readback->handle();
    auto submitted = run_graphics_commands(device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = target_image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_src;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1,
                               &region);
    });
    if (!submitted) {
        restore();
        return std::unexpected(submitted.error());
    }

    const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return px + (static_cast<std::size_t>(y) * extent.width + x) * 4;
    };
    // Inside the window: the solid red background (red dominant, opaque-ish).
    const std::uint8_t* inside = pixel(16, 16);
    // Outside: untouched clear colour.
    const std::uint8_t* outside = pixel(56, 56);
    restore();

    if (inside[0] < 128 || inside[0] <= inside[2]) {
        return fail("imgui self-test: window pixel is not the red window background");
    }
    if (outside[0] != 0 || outside[1] != 0 || outside[2] != 0) {
        return fail("imgui self-test: pixel outside the window was touched");
    }
    return {};
}

} // namespace engine::renderer
