// The editor UI composites in sRGB (gamma) space (issue #92). ImGui's colours
// and font antialiasing are authored for blending on the encoded values; the
// pass used to linearise vertex colours and blend into the sRGB swapchain in
// linear space, which thinned black text on white to grey (darkest pixel 64
// on the inspector's white Attenuation swatch).
//
// One UI frame — the editor's Inter text black on white and white on black,
// half-coverage quads, grey swatches, the Viewport panel's scene image — is
// drawn onto each kind of target the pass meets:
//   * a UNORM image (drawn directly);
//   * a mutable-format sRGB image with a registered UNORM view (the
//     VK_KHR_swapchain_mutable_format swapchain);
//   * a plain sRGB image (no such view: the copy-composite fallback),
// in editor mode (the pass clears) and overlay mode (it loads the scene the
// target already holds). Every kind must produce the same bytes.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#include <vulkan/vulkan.h>

#include "editor/ui/fonts.hpp"
#include "engine/renderer/imgui_pass.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace ui = engine::editor::ui;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D k_extent{192, 112};
constexpr const char* k_label = "Attenuation";

enum class Target : std::uint8_t { unorm, srgb_unorm_view, srgb_fallback };
enum class Mode : std::uint8_t { editor_clear, overlay_load };

const char* name(Target t) {
    switch (t) {
    case Target::unorm: return "UNORM target";
    case Target::srgb_unorm_view: return "sRGB target + UNORM view (mutable swapchain)";
    case Target::srgb_fallback: return "sRGB target, copy-composite fallback";
    }
    return "?";
}

// Creates a private ImGui context for its lifetime, restoring the previous
// one; the editor's global font set is cleared so it never points into this
// context's atlas afterwards.
class ScopedImGuiContext {
public:
    ScopedImGuiContext() : previous_(ImGui::GetCurrentContext()), ctx_(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(ctx_);
    }
    ~ScopedImGuiContext() {
        ui::set_fonts({});
        ImGui::DestroyContext(ctx_);
        ImGui::SetCurrentContext(previous_);
    }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
    ImGuiContext* previous_;
    ImGuiContext* ctx_;
};

// Pixel rectangles of the frame's features, [x0, x1) x [y0, y1).
struct Rect {
    int x0, y0, x1, y1;
};
// The two labels sit at the same x (one band below the other), so ImGui
// places their glyphs at identical sub-pixel offsets.
constexpr Rect k_light_band{0, 0, 192, 32};   // white, black label at (6, 8)
constexpr Rect k_dark_band{0, 32, 192, 64};   // black, white label at (6, 40)
constexpr Rect k_light_panel{0, 64, 96, 104}; // white: half coverage + greys
constexpr Rect k_dark_panel{96, 64, 192, 104}; // black: half coverage + viewport
constexpr Rect k_half_black{8, 72, 24, 88};   // black at alpha 128 over white
constexpr Rect k_half_white{104, 72, 120, 88}; // white at alpha 128 over black
constexpr std::array<int, 3> k_greys{37, 128, 200};
constexpr std::array<Rect, 3> k_grey_rects{{{32, 72, 48, 88}, {56, 72, 72, 88}, {76, 72, 92, 88}}};
constexpr Rect k_viewport_rect{128, 72, 176, 88};
constexpr glm::ivec3 k_viewport_bytes{100, 150, 200};
constexpr Rect k_over_scene{150, 104, 170, 112}; // black at alpha 128 over the scene row
constexpr glm::ivec2 k_scene_probe{10, 108};     // untouched by the UI

struct UiFrame {
    HostImage image;
    Rect      light_text{}; // where the black label is drawn
    Rect      dark_text{};  // where the white label is drawn
    bool      inter = false;
};

ImVec2 v2(int x, int y) { return ImVec2(static_cast<float>(x), static_cast<float>(y)); }

void fill(ImDrawList* list, const Rect& rc, ImU32 col) {
    list->AddRectFilled(v2(rc.x0, rc.y0), v2(rc.x1, rc.y1), col);
}

// Clears a colour target through its sRGB (or only) view.
void add_clear(rhi::RenderGraph& graph, rhi::ResourceHandle target, VkClearColorValue value) {
    (void)graph.add_pass("test_scene", rhi::PassType::graphics)
        .writes(target, rhi::ResourceUsage::color_attachment)
        .execute([target, value](rhi::PassContext& ctx) {
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = ctx.resolve(target).view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = value;
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, k_extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(ctx.cmd(), &rendering);
            vkCmdEndRendering(ctx.cmd());
        });
}

// The Viewport panel's scene image: UNORM bytes as TAA leaves them.
rhi::ResourceHandle add_viewport(rhi::RenderGraph& graph, VkFormat format) {
    const rhi::ResourceHandle vp = graph.create_transient_image(
        "test_viewport", format, k_extent,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    (void)graph.add_pass("test_viewport_fill", rhi::PassType::transfer)
        .writes(vp, rhi::ResourceUsage::transfer_dst)
        .execute([vp](rhi::PassContext& ctx) {
            VkClearColorValue value{};
            value.float32[0] = static_cast<float>(k_viewport_bytes.r) / 255.0F;
            value.float32[1] = static_cast<float>(k_viewport_bytes.g) / 255.0F;
            value.float32[2] = static_cast<float>(k_viewport_bytes.b) / 255.0F;
            value.float32[3] = 1.0F;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(ctx.cmd(), ctx.resolve(vp).image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        });
    return vp;
}

// Builds this test's UI frame with the editor's real body font (Inter at the
// inspector's size). Returns where the two labels landed.
void build_frame(const ui::FontSet& fonts, UiFrame& out) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = v2(static_cast<int>(k_extent.width), static_cast<int>(k_extent.height));
    ImGui::NewFrame();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    fill(list, k_light_band, IM_COL32(255, 255, 255, 255));
    fill(list, k_dark_band, IM_COL32(0, 0, 0, 255));
    fill(list, k_light_panel, IM_COL32(255, 255, 255, 255));
    fill(list, k_dark_panel, IM_COL32(0, 0, 0, 255));

    ImFont* font = fonts.fonts[static_cast<std::size_t>(ui::FontRole::body)];
    const float size = font->LegacySize;
    const ImVec2 extent = font->CalcTextSizeA(size, 1e6F, 0.0F, k_label);
    const auto place = [&](int x, int y, ImU32 col) {
        list->AddText(font, size, v2(x, y), col, k_label);
        return Rect{x, y, x + static_cast<int>(extent.x) + 1, y + static_cast<int>(extent.y) + 1};
    };
    out.light_text = place(6, 8, IM_COL32(0, 0, 0, 255));
    out.dark_text = place(6, 40, IM_COL32(255, 255, 255, 255));

    fill(list, k_half_black, IM_COL32(0, 0, 0, 128));
    fill(list, k_half_white, IM_COL32(255, 255, 255, 128));
    for (std::size_t i = 0; i < k_greys.size(); ++i) {
        const auto g = static_cast<unsigned>(k_greys[i]);
        fill(list, k_grey_rects[i], IM_COL32(g, g, g, 255));
    }
    list->AddImage(static_cast<ImTextureID>(r::ImGuiPass::viewport_texture_id),
                   v2(k_viewport_rect.x0, k_viewport_rect.y0),
                   v2(k_viewport_rect.x1, k_viewport_rect.y1));
    fill(list, k_over_scene, IM_COL32(0, 0, 0, 128));
    ImGui::Render();
}

std::expected<UiFrame, Error> render(quat::test::GpuContext& g, Target kind, Mode mode) {
    const ScopedImGuiContext scoped;
    UiFrame out;
    const ui::FontSet fonts = ui::load_fonts(*ImGui::GetIO().Fonts,
                                             std::filesystem::path(QUAT_PROJECT_ROOT) /
                                                 "assets" / "fonts",
                                             ui::DpiScale{});
    out.inter = fonts.inter;

    const VkFormat format =
        kind == Target::unorm ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
    auto pass = r::ImGuiPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir,
                                     format);
    if (!pass) return std::unexpected(pass.error());
    build_frame(fonts, out);

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    // The mutable-format image stands in for the swapchain: imported with
    // its sRGB view, the UNORM view registered beside it.
    rhi::GpuImage mutable_image;
    rhi::ImageView srgb_view;
    rhi::ImageView unorm_view;
    rhi::ResourceHandle target{};
    if (kind == Target::srgb_unorm_view) {
        auto image = g.allocator.create_image(format, k_extent, usage, 1, 1,
                                              VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        if (!image) return std::unexpected(image.error());
        mutable_image = std::move(*image);
        auto sv = rhi::create_image_view(g.device.handle(), mutable_image.handle(), format);
        auto uv = rhi::create_image_view(g.device.handle(), mutable_image.handle(),
                                         VK_FORMAT_R8G8B8A8_UNORM);
        if (!sv || !uv) return std::unexpected(Error{"test: image views"});
        srgb_view = std::move(*sv);
        unorm_view = std::move(*uv);
        target = graph.import_image("ui_target", mutable_image.handle(), srgb_view.handle(),
                                    format, k_extent, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        graph.set_unorm_view(target, unorm_view.handle());
    } else {
        target = graph.create_transient_image("ui_target", format, k_extent, usage);
    }
    // Overlay mode: the "scene" is opaque red, which encodes to (255, 0, 0)
    // in either format.
    if (mode == Mode::overlay_load) add_clear(graph, target, {{1.0F, 0.0F, 0.0F, 1.0F}});
    const rhi::ResourceHandle viewport = add_viewport(graph, VK_FORMAT_R8G8B8A8_UNORM);

    if (auto added = pass->add_to_graph(graph, target, k_extent, ImGui::GetDrawData(), viewport,
                                        /*clear=*/mode == Mode::editor_clear);
        !added) {
        return std::unexpected(added.error());
    }
    auto image = quat::test::render_to_host(g, graph, target, k_extent, 4);
    if (!image) return std::unexpected(image.error());
    out.image = std::move(*image);
    return out;
}

const std::expected<UiFrame, Error>& frame(quat::test::GpuContext& g, Target kind, Mode mode) {
    static std::array<std::array<std::expected<UiFrame, Error>, 2>, 3> cache{};
    static std::array<std::array<bool, 2>, 3> done{};
    const auto k = static_cast<std::size_t>(kind);
    const auto m = static_cast<std::size_t>(mode);
    if (!done[k][m]) {
        cache[k][m] = render(g, kind, mode);
        done[k][m] = true;
    }
    return cache[k][m];
}

constexpr std::array k_targets{Target::unorm, Target::srgb_unorm_view, Target::srgb_fallback};

int min_red(const HostImage& img, const Rect& rc) {
    int v = 255;
    for (int y = rc.y0; y < rc.y1; ++y) {
        for (int x = rc.x0; x < rc.x1; ++x) {
            v = std::min(v, img.rgba8(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)).r);
        }
    }
    return v;
}

int max_red(const HostImage& img, const Rect& rc) {
    int v = 0;
    for (int y = rc.y0; y < rc.y1; ++y) {
        for (int x = rc.x0; x < rc.x1; ++x) {
            v = std::max(v, img.rgba8(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)).r);
        }
    }
    return v;
}

glm::ivec3 rgb(const HostImage& img, const Rect& rc) {
    const auto x = static_cast<std::uint32_t>((rc.x0 + rc.x1) / 2);
    const auto y = static_cast<std::uint32_t>((rc.y0 + rc.y1) / 2);
    return glm::ivec3(img.rgba8(x, y));
}

void expect_near(glm::ivec3 got, glm::ivec3 want, int tolerance, const char* what) {
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(got[c], want[c], tolerance)
            << what << ": channel " << c << " is " << got[c] << ", want " << want[c];
    }
}

} // namespace

TEST(GpuImGuiSrgb, BlackTextOnWhiteReachesBlackAtTheGlyphCore) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        EXPECT_TRUE(f->inter) << "the editor's Inter font did not load";
        // Linear-space blending left the darkest glyph pixel at 64 (#92).
        EXPECT_LE(min_red(f->image, f->light_text), 10);
        // ... and the panel around the text stays white.
        EXPECT_EQ(rgb(f->image, Rect{170, 4, 180, 10}), glm::ivec3(255));
    }
}

TEST(GpuImGuiSrgb, WhiteTextOnBlackReachesWhiteAtTheGlyphCore) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        EXPECT_GE(max_red(f->image, f->dark_text), 245);
        EXPECT_EQ(rgb(f->image, Rect{170, 36, 180, 42}), glm::ivec3(0));
    }
}

// Gamma-space blending makes dark-on-light the complement of light-on-dark:
// a glyph pixel of coverage c is 255 (1 - c) on white and 255 c on black, so
// the label carries the same ink on either background and nearly every pixel
// pair sums to 255. (A few glyph-edge texels differ: the atlas has
// transparent black, not transparent white, around some glyphs, which only
// the white label picks up through bilinear filtering — as with ImGui's own
// backends.) Linear blending encodes both sides up — c = 0.5 shows as 188
// on white and 188 on black — so black text loses about half its ink and
// white text gains it.
TEST(GpuImGuiSrgb, TextWeightIsTheSameInBothThemes) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        const int dy = f->dark_text.y0 - f->light_text.y0;
        int partial = 0;
        int complementary = 0;
        long ink_on_white = 0;
        long ink_on_black = 0;
        for (int y = f->light_text.y0; y < f->light_text.y1; ++y) {
            for (int x = f->light_text.x0; x < f->light_text.x1; ++x) {
                const int on_white =
                    f->image.rgba8(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)).r;
                const int on_black = f->image
                                         .rgba8(static_cast<std::uint32_t>(x),
                                                static_cast<std::uint32_t>(y + dy))
                                         .r;
                ink_on_white += 255 - on_white;
                ink_on_black += on_black;
                if (on_white == 255) continue;
                ++partial;
                if (std::abs(on_white + on_black - 255) <= 2) ++complementary;
            }
        }
        ASSERT_GT(partial, 50) << "the label has glyph pixels to compare";
        EXPECT_GE(complementary * 10, partial * 9)
            << complementary << " of " << partial << " glyph pixels are complementary";
        const double ratio = static_cast<double>(ink_on_white) / static_cast<double>(ink_on_black);
        EXPECT_GT(ratio, 0.9) << "ink on white " << ink_on_white << ", on black " << ink_on_black;
        EXPECT_LT(ratio, 1.15) << "ink on white " << ink_on_white << ", on black " << ink_on_black;
    }
}

TEST(GpuImGuiSrgb, HalfCoverageBlendsInGammaSpace) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        // 255 * (1 - 128/255) and 255 * 128/255; linear blending gave 188.
        expect_near(rgb(f->image, k_half_black), glm::ivec3(127), 1, "black at alpha 128 on white");
        expect_near(rgb(f->image, k_half_white), glm::ivec3(128), 1, "white at alpha 128 on black");
    }
}

TEST(GpuImGuiSrgb, WidgetColoursRoundTripExactly) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        for (std::size_t i = 0; i < k_greys.size(); ++i) {
            EXPECT_EQ(rgb(f->image, k_grey_rects[i]), glm::ivec3(k_greys[i]))
                << "grey " << k_greys[i];
        }
    }
}

// The Viewport panel shows the tonemapped scene's encoded bytes unchanged —
// the same brightness as the game view, which presents those bytes.
TEST(GpuImGuiSrgb, ViewportImageBytesReachTheScreenUnchanged) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        EXPECT_EQ(rgb(f->image, k_viewport_rect), k_viewport_bytes);
    }
}

TEST(GpuImGuiSrgb, OverlayKeepsTheSceneAndBlendsOverIt) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::overlay_load);
        ASSERT_OK(f);
        const glm::ivec2 p = k_scene_probe;
        EXPECT_EQ(glm::ivec3(f->image.rgba8(static_cast<std::uint32_t>(p.x),
                                            static_cast<std::uint32_t>(p.y))),
                  glm::ivec3(255, 0, 0))
            << "scene pixels outside the UI keep their bytes";
        expect_near(rgb(f->image, k_over_scene), glm::ivec3(127, 0, 0), 1,
                    "black at alpha 128 over the red scene");
        EXPECT_LE(min_red(f->image, f->light_text), 10);
    }
}

TEST(GpuImGuiSrgb, EditorModeClearsBehindTheUi) {
    QUAT_REQUIRE_GPU(g);
    for (const Target kind : k_targets) {
        SCOPED_TRACE(name(kind));
        const auto& f = frame(*g, kind, Mode::editor_clear);
        ASSERT_OK(f);
        const glm::ivec2 p = k_scene_probe;
        // The clear value is stored as is (sRGB-encoded, like the theme).
        expect_near(glm::ivec3(f->image.rgba8(static_cast<std::uint32_t>(p.x),
                                              static_cast<std::uint32_t>(p.y))),
                    glm::ivec3(15, 15, 18), 1, "editor background");
    }
}

TEST(GpuImGuiSrgb, AnSrgbViewportWithoutAUnormViewIsRefused) {
    QUAT_REQUIRE_GPU(g);
    const ScopedImGuiContext scoped;
    auto pass = r::ImGuiPass::create(g->device, g->allocator, g->cache, g->transfer,
                                     g->shader_dir, VK_FORMAT_R8G8B8A8_UNORM);
    ASSERT_OK(pass);
    ImGui::GetIO().DisplaySize = ImVec2(64.0F, 64.0F);
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddImage(
        static_cast<ImTextureID>(r::ImGuiPass::viewport_texture_id), ImVec2(0, 0),
        ImVec2(32, 32));
    ImGui::Render();
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle target = graph.create_transient_image(
        "ui_target", VK_FORMAT_R8G8B8A8_UNORM, {64, 64}, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const rhi::ResourceHandle viewport = graph.create_transient_image(
        "viewport", VK_FORMAT_R8G8B8A8_SRGB, {64, 64}, VK_IMAGE_USAGE_SAMPLED_BIT);
    const auto added =
        pass->add_to_graph(graph, target, {64, 64}, ImGui::GetDrawData(), viewport, true);
    ASSERT_FALSE(added.has_value());
    EXPECT_NE(added.error().message.find("UNORM view"), std::string::npos)
        << added.error().message;
}

#endif // ENGINE_EDITOR
