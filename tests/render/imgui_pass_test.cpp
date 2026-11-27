// The ImGui overlay pass (editor builds): a borderless solid-red window
// covering the upper-left quadrant is rasterised over a cleared target, and
// pixels outside it are left untouched. The test drives a private ImGui
// context so it never disturbs the editor's.

#ifdef ENGINE_EDITOR

#include <expected>

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

#endif // ENGINE_EDITOR
