#include "theme.hpp"

#include <cmath>
#include <cstddef>

#include <imgui.h>

#include "metrics.hpp"

namespace engine::editor::ui {

namespace {

// Palettes. Every text/surface pairing is checked against WCAG AA by
// tests/editor/ui_theme_test.cpp; see docs/editor-ux.md for the rationale.
constexpr Theme k_dark{
    .id = ThemeId::dark,
    .name = "Dark",
    .bg0 = rgb(0x131417),
    .bg1 = rgb(0x1B1C20),
    .bg2 = rgb(0x25272C),
    .bg3 = rgb(0x30333A),
    .text = rgb(0xE4E6EA),
    .text_secondary = rgb(0xA2A7B1),
    .text_disabled = rgb(0x6E737D),
    .accent = rgb(0x2B6CD4),
    .accent_hover = rgb(0x2F70D9),
    .accent_active = rgb(0x245DB8),
    .on_accent = rgb(0xFFFFFF),
    .accent_text = rgb(0x6EA8FE),
    .border = rgb(0x2E3036),
    .separator = rgb(0x2A2C31),
    .success = rgb(0x4CC38A),
    .warning = rgb(0xE8B04B),
    .error = rgb(0xF26D66),
    .info = rgb(0x62AEF5),
    .selection = rgb(0x2B6CD4, 0.40F),
    .axis_x = rgb(0xE5484D),
    .axis_y = rgb(0x5DBB46),
    .axis_z = rgb(0x3D8CF0),
};

constexpr Theme k_light{
    .id = ThemeId::light,
    .name = "Light",
    .bg0 = rgb(0xDDE0E5),
    .bg1 = rgb(0xF3F4F6),
    .bg2 = rgb(0xFFFFFF),
    .bg3 = rgb(0xE6E8EC),
    .text = rgb(0x1C1F24),
    .text_secondary = rgb(0x4A505A),
    .text_disabled = rgb(0x737983),
    .accent = rgb(0x2160C4),
    .accent_hover = rgb(0x2A6BD0),
    .accent_active = rgb(0x1B52A8),
    .on_accent = rgb(0xFFFFFF),
    .accent_text = rgb(0x1D57B3),
    .border = rgb(0xC9CDD3),
    .separator = rgb(0xD5D8DD),
    .success = rgb(0x176B42),
    .warning = rgb(0x825200),
    .error = rgb(0xB42A2A),
    .info = rgb(0x1F63B5),
    .selection = rgb(0x2160C4, 0.22F),
    .axis_x = rgb(0xC8323A),
    .axis_y = rgb(0x3A8A1E),
    .axis_z = rgb(0x2466C8),
};

const Theme* g_current = &k_dark;
float        g_scale = 1.0F;

[[nodiscard]] float linear_channel(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

} // namespace

const Theme& dark_theme() { return k_dark; }
const Theme& light_theme() { return k_light; }
const Theme& theme(ThemeId id) { return id == ThemeId::light ? k_light : k_dark; }
const Theme& tokens() { return *g_current; }

void  set_ui_scale(float scale) { g_scale = scale; }
float ui_scale() { return g_scale; }
float scaled(float logical_px) { return logical_px * g_scale; }

float relative_luminance(Rgba c) {
    return 0.2126F * linear_channel(c.r) + 0.7152F * linear_channel(c.g) +
           0.0722F * linear_channel(c.b);
}

Rgba composite(Rgba fg, Rgba bg) {
    const float a = fg.a;
    return {fg.r * a + bg.r * (1.0F - a), fg.g * a + bg.g * (1.0F - a),
            fg.b * a + bg.b * (1.0F - a), 1.0F};
}

float contrast_ratio(Rgba fg, Rgba bg) {
    const Rgba solid_fg = composite(fg, with_alpha(bg, 1.0F));
    const float l1 = relative_luminance(solid_fg);
    const float l2 = relative_luminance(bg);
    const float hi = l1 > l2 ? l1 : l2;
    const float lo = l1 > l2 ? l2 : l1;
    return (hi + 0.05F) / (lo + 0.05F);
}

std::size_t contrast_pairs(const Theme& t, std::span<ContrastPair> out) {
    constexpr float aa = 4.5F;      // WCAG AA, normal text
    constexpr float ui_min = 3.0F;  // WCAG non-text contrast / inactive text floor
    const Rgba selected_row = composite(t.selection, t.bg1);
    const ContrastPair pairs[] = {
        {"text on bg0", t.text, t.bg0, aa},
        {"text on bg1", t.text, t.bg1, aa},
        {"text on bg2", t.text, t.bg2, aa},
        {"text on bg3", t.text, t.bg3, aa},
        {"secondary text on bg0", t.text_secondary, t.bg0, aa},
        {"secondary text on bg1", t.text_secondary, t.bg1, aa},
        {"secondary text on bg2", t.text_secondary, t.bg2, aa},
        {"disabled text on bg1", t.text_disabled, t.bg1, ui_min},
        {"disabled text on bg2", t.text_disabled, t.bg2, ui_min},
        {"accent text on bg0", t.accent_text, t.bg0, aa},
        {"accent text on bg1", t.accent_text, t.bg1, aa},
        {"accent text on bg2", t.accent_text, t.bg2, aa},
        {"on-accent on accent", t.on_accent, t.accent, aa},
        {"on-accent on accent hover", t.on_accent, t.accent_hover, aa},
        {"on-accent on accent active", t.on_accent, t.accent_active, aa},
        {"success on bg1", t.success, t.bg1, aa},
        {"warning on bg1", t.warning, t.bg1, aa},
        {"error on bg1", t.error, t.bg1, aa},
        {"info on bg1", t.info, t.bg1, aa},
        {"success on bg0", t.success, t.bg0, aa},
        {"warning on bg0", t.warning, t.bg0, aa},
        {"error on bg0", t.error, t.bg0, aa},
        {"info on bg0", t.info, t.bg0, aa},
        {"text on selected row", t.text, selected_row, aa},
        {"X axis marker on bg2", t.axis_x, t.bg2, ui_min},
        {"Y axis marker on bg2", t.axis_y, t.bg2, ui_min},
        {"Z axis marker on bg2", t.axis_z, t.bg2, ui_min},
    };
    std::size_t n = 0;
    for (const ContrastPair& p : pairs) {
        if (n == out.size()) break;
        out[n++] = p;
    }
    return n;
}

ImVec4 to_imvec4(Rgba c) { return {c.r, c.g, c.b, c.a}; }

std::uint32_t to_u32(Rgba c) { return ImGui::ColorConvertFloat4ToU32(to_imvec4(c)); }

void apply_theme(const Theme& t, float ui_scale_value) {
    g_current = &t;
    g_scale = ui_scale_value;

    ImGuiStyle& style = ImGui::GetStyle();
    const float font_size_base = style.FontSizeBase;
    style = ImGuiStyle(); // reset metrics before scaling (apply_theme may run again)
    style.FontSizeBase = font_size_base;

    style.WindowPadding = ImVec2(Spacing::xs, Spacing::xs);
    style.FramePadding = ImVec2(Spacing::xs, Sizes::frame_pad_y);
    style.CellPadding = ImVec2(Spacing::xs * 0.75F, 3.0F);
    style.ItemSpacing = ImVec2(Spacing::xs, Spacing::xxs + 1.0F);
    style.ItemInnerSpacing = ImVec2(Spacing::xxs + 2.0F, Spacing::xxs);
    style.TouchExtraPadding = ImVec2(0.0F, 0.0F);
    style.IndentSpacing = Spacing::md - 2.0F;
    style.ScrollbarSize = Spacing::sm;
    style.GrabMinSize = Spacing::xs + 2.0F;

    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = 2.0F;
    style.DockingSeparatorSize = 2.0F;
    style.SeparatorTextBorderSize = 1.0F;

    style.WindowRounding = Radius::lg;
    style.ChildRounding = Radius::md;
    style.FrameRounding = Radius::md;
    style.PopupRounding = Radius::lg;
    style.ScrollbarRounding = Radius::lg;
    style.GrabRounding = Radius::sm;
    style.TabRounding = Radius::md;

    style.WindowTitleAlign = ImVec2(0.0F, 0.5F);
    style.WindowMenuButtonPosition = ImGuiDir_None; // dock menu via the tab context
    style.ColorButtonPosition = ImGuiDir_Left;
    style.SeparatorTextPadding = ImVec2(Spacing::md, Spacing::xxs - 1.0F);
    style.DisplaySafeAreaPadding = ImVec2(0.0F, 0.0F);

    style.ScaleAllSizes(ui_scale_value);

    ImVec4* c = style.Colors;
    const auto v = [](Rgba col) { return to_imvec4(col); };
    const auto a = [](Rgba col, float alpha) { return to_imvec4(with_alpha(col, alpha)); };
    const ImVec4 clear(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_Text] = v(t.text);
    c[ImGuiCol_TextDisabled] = v(t.text_disabled);
    c[ImGuiCol_WindowBg] = v(t.bg1);
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = v(t.bg2);
    c[ImGuiCol_Border] = v(t.border);
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = v(t.bg2);
    c[ImGuiCol_FrameBgHovered] = v(t.bg3);
    c[ImGuiCol_FrameBgActive] = v(t.bg3);
    c[ImGuiCol_TitleBg] = v(t.bg0);
    c[ImGuiCol_TitleBgActive] = v(t.bg0);
    c[ImGuiCol_TitleBgCollapsed] = v(t.bg0);
    c[ImGuiCol_MenuBarBg] = v(t.bg0);
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = v(t.bg3);
    c[ImGuiCol_ScrollbarGrabHovered] = a(t.text_disabled, 0.8F);
    c[ImGuiCol_ScrollbarGrabActive] = v(t.text_disabled);
    c[ImGuiCol_CheckMark] = v(t.accent_text);
    c[ImGuiCol_SliderGrab] = v(t.accent);
    c[ImGuiCol_SliderGrabActive] = v(t.accent_hover);
    c[ImGuiCol_Button] = v(t.bg2);
    c[ImGuiCol_ButtonHovered] = v(t.bg3);
    c[ImGuiCol_ButtonActive] = a(t.accent, 0.55F);
    c[ImGuiCol_Header] = v(t.selection);
    c[ImGuiCol_HeaderHovered] = a(t.text, 0.06F);
    c[ImGuiCol_HeaderActive] = a(t.accent, 0.55F);
    c[ImGuiCol_Separator] = v(t.separator);
    c[ImGuiCol_SeparatorHovered] = a(t.accent, 0.7F);
    c[ImGuiCol_SeparatorActive] = v(t.accent);
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = a(t.accent, 0.6F);
    c[ImGuiCol_ResizeGripActive] = v(t.accent);
    c[ImGuiCol_InputTextCursor] = v(t.text);
    c[ImGuiCol_TabHovered] = v(t.bg3);
    c[ImGuiCol_Tab] = v(t.bg0);
    c[ImGuiCol_TabSelected] = v(t.bg1);
    c[ImGuiCol_TabSelectedOverline] = v(t.accent);
    c[ImGuiCol_TabDimmed] = v(t.bg0);
    c[ImGuiCol_TabDimmedSelected] = v(t.bg1);
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_DockingPreview] = a(t.accent, 0.45F);
    c[ImGuiCol_DockingEmptyBg] = v(t.bg0);
    c[ImGuiCol_PlotLines] = v(t.accent_text);
    c[ImGuiCol_PlotLinesHovered] = v(t.warning);
    c[ImGuiCol_PlotHistogram] = v(t.accent_text);
    c[ImGuiCol_PlotHistogramHovered] = v(t.warning);
    c[ImGuiCol_TableHeaderBg] = v(t.bg2);
    c[ImGuiCol_TableBorderStrong] = v(t.border);
    c[ImGuiCol_TableBorderLight] = v(t.separator);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = a(t.text, 0.025F);
    c[ImGuiCol_TextLink] = v(t.accent_text);
    c[ImGuiCol_TextSelectedBg] = a(t.accent, 0.35F);
    c[ImGuiCol_TreeLines] = v(t.separator);
    c[ImGuiCol_DragDropTarget] = v(t.accent_text);
    c[ImGuiCol_DragDropTargetBg] = a(t.accent, 0.12F);
    c[ImGuiCol_UnsavedMarker] = v(t.text);
    c[ImGuiCol_NavCursor] = v(t.accent_text);
    c[ImGuiCol_NavWindowingHighlight] = a(t.text, 0.7F);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.35F);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.45F);
}

} // namespace engine::editor::ui
