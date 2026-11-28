// The ImGui overlay pass (editor builds): a borderless solid-red window
// covering the upper-left quadrant is rasterised over a cleared target, and
// pixels outside it are left untouched; a draw list past 65535 vertices is
// split with VtxOffset and drawn in place (#10). The tests drive a private
// ImGui context so they never disturb the editor's.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <expected>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/imgui_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D k_extent{64, 64};
constexpr VkFormat   k_format = VK_FORMAT_R8G8B8A8_UNORM;

// Creates a private ImGui context for its lifetime, restoring the previous one.
class ScopedImGuiContext {
public:
    ScopedImGuiContext() : previous_(ImGui::GetCurrentContext()), ctx_(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(ctx_);
    }
    ~ScopedImGuiContext() {
        ImGui::DestroyContext(ctx_);
        ImGui::SetCurrentContext(previous_);
    }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
    ImGuiContext* previous_;
    ImGuiContext* ctx_;
};

std::expected<HostImage, Error> render_red_window(quat::test::GpuContext& g) {
    const ScopedImGuiContext scoped;
    auto pass = r::ImGuiPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir,
                                     k_format);
    if (!pass) return std::unexpected(pass.error());

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(k_extent.width), static_cast<float>(k_extent.height));
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(32, 32));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0F, 0.0F, 0.0F, 1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::Begin("imgui_pass_test", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Render();

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle target = graph.create_transient_image(
        "ui_target", k_format, k_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    // The UI pass loads (LOAD_OP_LOAD), so clear the transient underneath it.
    (void)graph.add_pass("clear", rhi::PassType::graphics)
        .writes(target, rhi::ResourceUsage::color_attachment)
        .execute([target](rhi::PassContext& ctx) {
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = ctx.resolve(target).view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, k_extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(ctx.cmd(), &rendering);
            vkCmdEndRendering(ctx.cmd());
        });
    if (auto added = pass->add_to_graph(graph, target, k_extent, ImGui::GetDrawData()); !added) {
        return std::unexpected(added.error());
    }
    return quat::test::render_to_host(g, graph, target, k_extent, 4);
}

const std::expected<HostImage, Error>& ui_frame(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = render_red_window(g);
    return image;
}

// One draw list past the 16-bit index range (#10): red lines over the top
// half, then — only after the 65536th vertex — green lines over the bottom
// half. The green lines' indices are relative to a non-zero VtxOffset, so a
// pass ignoring it would draw them over the red vertices instead.
constexpr int k_red_lines = 18'000;  // >= 4 vertices each: past 65535 alone
constexpr int k_green_lines = 2'000;

struct BigListFrame {
    HostImage image;
    int       max_list_vertices = 0;
    int       commands_with_vtx_offset = 0;
    bool      indices_in_range = true; // every index + VtxOffset < list size
};

std::expected<BigListFrame, Error> render_big_draw_list(quat::test::GpuContext& g) {
    const ScopedImGuiContext scoped;
    auto pass = r::ImGuiPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir,
                                     k_format);
    if (!pass) return std::unexpected(pass.error());

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(k_extent.width), static_cast<float>(k_extent.height));
    ImGui::NewFrame();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float width = static_cast<float>(k_extent.width);
    for (int i = 0; i < k_red_lines; ++i) {
        const float y = 4.0F + static_cast<float>(i % 24); // rows 4..27
        list->AddLine(ImVec2(0.0F, y), ImVec2(width, y), IM_COL32(255, 0, 0, 255), 1.0F);
    }
    for (int i = 0; i < k_green_lines; ++i) {
        const float y = 36.0F + static_cast<float>(i % 24); // rows 36..59
        list->AddLine(ImVec2(0.0F, y), ImVec2(width, y), IM_COL32(0, 255, 0, 255), 1.0F);
    }
    ImGui::Render();

    BigListFrame out;
    const ImDrawData* draw_data = ImGui::GetDrawData();
    for (int li = 0; li < draw_data->CmdListsCount; ++li) {
        const ImDrawList* dl = draw_data->CmdLists[li];
        out.max_list_vertices = std::max(out.max_list_vertices, dl->VtxBuffer.Size);
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.VtxOffset != 0) ++out.commands_with_vtx_offset;
            for (unsigned int k = 0; k < cmd.ElemCount; ++k) {
                const unsigned int index = dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + k)];
                if (cmd.VtxOffset + index >= static_cast<unsigned int>(dl->VtxBuffer.Size)) {
                    out.indices_in_range = false;
                }
            }
        }
    }

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle target = graph.create_transient_image(
        "ui_target", k_format, k_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto added = pass->add_to_graph(graph, target, k_extent, draw_data, {}, /*clear=*/true);
        !added) {
        return std::unexpected(added.error());
    }
    auto image = quat::test::render_to_host(g, graph, target, k_extent, 4);
    if (!image) return std::unexpected(image.error());
    out.image = std::move(*image);
    return out;
}

const std::expected<BigListFrame, Error>& big_list_frame(quat::test::GpuContext& g) {
    static const std::expected<BigListFrame, Error> frame = render_big_draw_list(g);
    return frame;
}

} // namespace

TEST(GpuImGuiPass, WindowPixelIsTheRedBackground) {
    QUAT_REQUIRE_GPU(g);
    const auto& ui = ui_frame(*g);
    ASSERT_OK(ui);
    const glm::ivec4 inside = ui->rgba8(16, 16);
    EXPECT_GE(inside.r, 128);
    EXPECT_GT(inside.r, inside.b) << "window pixel is not red-dominant";
}

TEST(GpuImGuiPass, PixelsOutsideTheWindowAreUntouched) {
    QUAT_REQUIRE_GPU(g);
    const auto& ui = ui_frame(*g);
    ASSERT_OK(ui);
    EXPECT_EQ(glm::ivec3(ui->rgba8(56, 56)), glm::ivec3(0));
}

TEST(GpuImGuiPass, DrawListPast16BitIndicesSplitsWithVtxOffset) {
    QUAT_REQUIRE_GPU(g);
    const auto& frame = big_list_frame(*g);
    ASSERT_OK(frame);
    EXPECT_GT(frame->max_list_vertices, 65535);
    EXPECT_GT(frame->commands_with_vtx_offset, 0)
        << "ImGui did not split the list: RendererHasVtxOffset is not set";
    EXPECT_TRUE(frame->indices_in_range);
}

TEST(GpuImGuiPass, CommandsPastTheFirst64kVerticesDrawWhereTheyBelong) {
    QUAT_REQUIRE_GPU(g);
    const auto& frame = big_list_frame(*g);
    ASSERT_OK(frame);
    const glm::ivec4 top = frame->image.rgba8(32, 16);
    EXPECT_GE(top.r, 128);
    EXPECT_GT(top.r, top.g) << "top half is not red";
    const glm::ivec4 bottom = frame->image.rgba8(32, 48);
    EXPECT_GE(bottom.g, 128);
    EXPECT_GT(bottom.g, bottom.r) << "lines after the VtxOffset split are missing or misplaced";
}

#endif // ENGINE_EDITOR
