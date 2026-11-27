// Widget kit: text, buttons, toolbars, search fields, headers, empty states.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cctype>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // ButtonBehavior / ItemAdd for the custom header
#include <imgui_stdlib.h>

#include "fonts.hpp"
#include "icons.hpp"
#include "metrics.hpp"
#include "widgets.hpp"

namespace engine::editor::ui {

namespace {

// Centre `text` of the current font in [min, max].
void draw_centered(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(std::floor(min.x + (max.x - min.x - size.x) * 0.5F),
                       std::floor(min.y + (max.y - min.y - size.y) * 0.5F)),
                col, text);
}

} // namespace

// ---- Text ------------------------------------------------------------------

std::string label(const char* icon, std::string_view text) {
    const char* glyph = ic(icon);
    std::string out;
    out.reserve(text.size() + 8);
    if (glyph[0] != '\0') {
        out += glyph;
        out += "  ";
    }
    out += text;
    return out;
}

void text_secondary(std::string_view text) { text_colored(tokens().text_secondary, text); }

void text_colored(Rgba color, std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(color));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

void item_tooltip(const char* text, const char* shortcut) {
    if (text == nullptr || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(scaled(320.0F));
    ImGui::TextUnformatted(text);
    if (shortcut != nullptr && shortcut[0] != '\0') {
        ImGui::SameLine(0.0F, scaled(Spacing::sm));
        text_colored(tokens().text_secondary, shortcut);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ---- Buttons ---------------------------------------------------------------

bool button(const char* text, ButtonKind kind, ImVec2 size) {
    const Theme& t = tokens();
    int pushed = 0;
    const auto push = [&](ImGuiCol idx, Rgba c) {
        ImGui::PushStyleColor(idx, to_imvec4(c));
        ++pushed;
    };
    switch (kind) {
    case ButtonKind::primary:
        push(ImGuiCol_Button, t.accent);
        push(ImGuiCol_ButtonHovered, t.accent_hover);
        push(ImGuiCol_ButtonActive, t.accent_active);
        push(ImGuiCol_Text, t.on_accent);
        push(ImGuiCol_Border, t.accent_active);
        break;
    case ButtonKind::danger:
        push(ImGuiCol_Text, t.error);
        break;
    case ButtonKind::ghost:
        push(ImGuiCol_Button, with_alpha(t.bg2, 0.0F));
        push(ImGuiCol_Border, with_alpha(t.border, 0.0F));
        break;
    case ButtonKind::secondary:
        break;
    }
    const bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleColor(pushed);
    return clicked;
}

bool icon_button(const char* id, const char* icon, const char* tooltip, const char* shortcut,
                 bool enabled) {
    return toggle_button(id, icon, false, tooltip, shortcut, enabled);
}

bool toggle_button(const char* id, const char* icon, bool active, const char* tooltip,
                   const char* shortcut, bool enabled) {
    const Theme& t = tokens();
    const float side = std::round(ImGui::GetFrameHeight());
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, to_imvec4(t.accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(t.accent_hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, to_imvec4(t.accent_active));
        ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(t.on_accent));
    } else {
        // Inactive buttons keep the caller's text colour (status-coloured
        // icons such as Play / Stop).
        ImGui::PushStyleColor(ImGuiCol_Button, to_imvec4(with_alpha(t.bg2, 0.0F)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(t.bg3));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, to_imvec4(with_alpha(t.accent, 0.45F)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
    }
    const char* glyph = ic(icon);
    // Without the icon font, fall back to the id's first letter so the
    // button is still usable.
    char fallback[2] = {id[0] != '\0' ? static_cast<char>(std::toupper(id[0])) : '?', '\0'};
    const bool clicked = ImGui::Button(glyph[0] != '\0' ? glyph : fallback, ImVec2(side, side));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    if (tooltip != nullptr) {
        // Tooltips also explain why a disabled control is disabled.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (ImGui::BeginTooltip()) {
                ImGui::TextUnformatted(tooltip);
                if (shortcut != nullptr && shortcut[0] != '\0') {
                    ImGui::SameLine(0.0F, scaled(Spacing::sm));
                    text_colored(t.text_secondary, shortcut);
                }
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::PopID();
    return clicked && enabled;
}

void begin_toolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(scaled(2.0F), scaled(Spacing::xxs)));
}

void toolbar_separator() {
    ImGui::SameLine(0.0F, scaled(Spacing::xs));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.2F), ImVec2(p.x, p.y + h * 0.8F),
                                        to_u32(tokens().separator), scaled(1.0F));
    ImGui::Dummy(ImVec2(scaled(1.0F), h));
    ImGui::SameLine(0.0F, scaled(Spacing::xs));
}

void end_toolbar() { ImGui::PopStyleVar(); }

// ---- Search ----------------------------------------------------------------

bool matches_filter(std::string_view text, std::string_view filter) {
    if (filter.empty()) return true;
    if (filter.size() > text.size()) return false;
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + filter.size() <= text.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < filter.size() && ok; ++k) {
            ok = lower(text[i + k]) == lower(filter[k]);
        }
        if (ok) return true;
    }
    return false;
}

bool search_field(const char* id, std::string& text, const char* hint, float width) {
    const Theme& t = tokens();
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float icon_w = ImGui::GetFontSize();
    const float w = width > 0.0F ? width : ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x * 2.0F + icon_w, style.FramePadding.y));
    ImGui::SetNextItemAllowOverlap();
    bool changed = ImGui::InputTextWithHint("##search", hint, &text);
    ImGui::PopStyleVar();
    const bool focused = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* glyph = ic(icon::search);
    if (glyph[0] != '\0') {
        draw_centered(dl, ImVec2(min.x + style.FramePadding.x, min.y),
                      ImVec2(min.x + style.FramePadding.x + icon_w, max.y),
                      to_u32(t.text_secondary), glyph);
    }
    if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape) && !text.empty()) {
        text.clear();
        changed = true;
    }
    if (!text.empty()) {
        const float side = max.y - min.y;
        ImGui::SetCursorScreenPos(ImVec2(max.x - side, min.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(t.bg3));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        const char* x = ic(icon::x);
        if (ImGui::Button(x[0] != '\0' ? x : "x", ImVec2(side, side))) {
            text.clear();
            changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        item_tooltip("Clear", "Esc");
    }
    ImGui::PopID();
    return changed;
}

// ---- Headers ---------------------------------------------------------------

ComponentHeader component_header(const ComponentHeaderSpec& spec) {
    const Theme& t = tokens();
    ComponentHeader out;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return out;

    ImGui::PushID(spec.id);
    const ImGuiID id = ImGui::GetID("##header");
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool open = storage->GetBool(id, spec.default_open);

    const float h = std::round(scaled(Sizes::row_height + 4.0F));
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float y0 = ImGui::GetCursorScreenPos().y;
    const float full_w = ImGui::GetContentRegionAvail().x;
    const ImRect bb(ImVec2(x0, y0), ImVec2(x0 + full_w, y0 + h));
    ImGui::ItemSize(bb.GetSize());
    ImGui::SetNextItemAllowOverlap(); // the enable toggle and the menu sit on top
    if (!ImGui::ItemAdd(bb, id)) {
        ImGui::PopID();
        out.open = open;
        return out;
    }

    // Right-aligned controls: [enable] [⋮]. The toggle area is the rest.
    const float pad = scaled(Spacing::xs);
    const float btn = h - scaled(4.0F);
    const float menu_x = bb.Max.x - pad * 0.5F - btn;
    ImRect toggle_bb = bb;
    toggle_bb.Max.x = menu_x - scaled(2.0F);
    bool hovered = false;
    bool held = false;
    if (ImGui::ButtonBehavior(toggle_bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick)) {
        open = !open;
        storage->SetBool(id, open);
    }
    const bool menu_requested = ImGui::IsMouseHoveringRect(bb.Min, bb.Max) &&
                                ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                                ImGui::IsWindowHovered();

    ImDrawList* dl = window->DrawList;
    const Rgba fill = hovered ? t.bg3 : t.bg2;
    dl->AddRectFilled(bb.Min, bb.Max, to_u32(fill), scaled(Radius::md));
    float x = bb.Min.x + pad;
    const float mid_y = bb.Min.y + h * 0.5F;
    // Disclosure chevron.
    const char* chevron = ic(open ? icon::chevron_down : icon::chevron_right);
    if (chevron[0] != '\0') {
        draw_centered(dl, ImVec2(x, bb.Min.y), ImVec2(x + ImGui::GetFontSize(), bb.Max.y),
                      to_u32(t.text_secondary), chevron);
    } else {
        ImGui::RenderArrow(dl, ImVec2(x, mid_y - ImGui::GetFontSize() * 0.5F),
                           to_u32(t.text_secondary), open ? ImGuiDir_Down : ImGuiDir_Right, 0.8F);
    }
    x += ImGui::GetFontSize() + scaled(Spacing::xxs);
    // Enable toggle sits before the icon (Unity / Godot convention).
    if (spec.enabled != nullptr) {
        ImGui::SetCursorScreenPos(ImVec2(x, mid_y - ImGui::GetFrameHeight() * 0.5F));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(scaled(3.0F), scaled(3.0F)));
        ImGui::Checkbox("##enabled", spec.enabled);
        ImGui::PopStyleVar();
        item_tooltip(*spec.enabled ? "Enabled (click to disable)" : "Disabled (click to enable)");
        x = ImGui::GetItemRectMax().x + scaled(Spacing::xs);
    }
    const char* glyph = ic(spec.icon);
    if (glyph[0] != '\0') {
        draw_centered(dl, ImVec2(x, bb.Min.y), ImVec2(x + ImGui::GetFontSize(), bb.Max.y),
                      to_u32(t.accent_text), glyph);
        x += ImGui::GetFontSize() + scaled(Spacing::xs);
    }
    {
        FontScope heading(FontRole::heading);
        const ImVec2 ts = ImGui::CalcTextSize(spec.title);
        const Rgba title_col = (spec.enabled != nullptr && !*spec.enabled) ? t.text_disabled : t.text;
        dl->PushClipRect(bb.Min, ImVec2(menu_x, bb.Max.y), true);
        dl->AddText(ImVec2(x, std::floor(mid_y - ts.y * 0.5F)), to_u32(title_col), spec.title);
        dl->PopClipRect();
    }
    if (spec.tooltip != nullptr && hovered && !held) {
        ImGui::SetItemTooltip("%s", spec.tooltip);
    }

    // ⋮ menu.
    ImGui::SetCursorScreenPos(ImVec2(menu_x, bb.Min.y + (h - btn) * 0.5F));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(t.bg3));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
    const char* dots = ic(icon::ellipsis_vertical);
    const bool menu_clicked = ImGui::Button(dots[0] != '\0' ? dots : "...", ImVec2(btn, btn));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    item_tooltip("Component actions");
    if (menu_clicked || menu_requested) ImGui::OpenPopup("##component_menu");
    if (ImGui::BeginPopup("##component_menu")) {
        if (ImGui::MenuItem(label(icon::rotate_ccw, "Reset to defaults").c_str(), nullptr, false,
                            spec.can_reset)) {
            out.action = HeaderAction::reset;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(label(icon::copy, "Copy values").c_str(), nullptr, false,
                            spec.can_copy)) {
            out.action = HeaderAction::copy;
        }
        if (ImGui::MenuItem(label(icon::clipboard_paste, "Paste values").c_str(), nullptr, false,
                            spec.can_paste)) {
            out.action = HeaderAction::paste;
        }
        if (spec.removable) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(t.error));
            if (ImGui::MenuItem(label(icon::trash_2, "Remove component").c_str())) {
                out.action = HeaderAction::remove;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    // Restore the layout cursor below the header.
    ImGui::SetCursorScreenPos(ImVec2(x0, bb.Max.y + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0F, 0.0F));
    ImGui::PopID();
    out.open = open;
    return out;
}

bool section_header(const char* text, bool default_open) {
    const Theme& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_Header, to_imvec4(t.bg2));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, to_imvec4(t.bg3));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, to_imvec4(t.bg3));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(scaled(Spacing::xs), scaled(Sizes::frame_pad_y + 1.0F)));
    push_font(FontRole::heading);
    const bool open = ImGui::CollapsingHeader(
        text, default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
    pop_font();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return open;
}

// ---- Empty states ----------------------------------------------------------

bool empty_state(const char* icon_glyph, const char* title, const char* hint, const char* action) {
    const Theme& t = tokens();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float wrap = std::min(avail.x - scaled(Spacing::lg), scaled(300.0F));
    // Measure the block to centre it vertically (in the upper third when tall).
    const char* glyph = ic(icon_glyph);
    float block = 0.0F;
    if (glyph[0] != '\0') {
        FontScope big(FontRole::icon_large);
        block += ImGui::GetFontSize() + scaled(Spacing::xs);
    }
    block += ImGui::GetTextLineHeight();
    ImVec2 hint_size(0.0F, 0.0F);
    if (hint != nullptr) {
        hint_size = ImGui::CalcTextSize(hint, nullptr, false, wrap);
        block += scaled(Spacing::xxs) + hint_size.y;
    }
    if (action != nullptr) block += scaled(Spacing::sm) + ImGui::GetFrameHeight();
    const float start_y =
        ImGui::GetCursorPosY() + std::max(scaled(Spacing::lg), (avail.y - block) * 0.38F);
    ImGui::SetCursorPosY(start_y);

    const float cx = ImGui::GetCursorPosX() + avail.x * 0.5F;
    if (glyph[0] != '\0') {
        FontScope big(FontRole::icon_large);
        const ImVec2 sz = ImGui::CalcTextSize(glyph);
        ImGui::SetCursorPosX(cx - sz.x * 0.5F);
        text_colored(t.text_disabled, glyph);
        ImGui::Dummy(ImVec2(0.0F, scaled(Spacing::xxs)));
    }
    {
        FontScope heading(FontRole::heading);
        const ImVec2 sz = ImGui::CalcTextSize(title);
        ImGui::SetCursorPosX(std::max(cx - sz.x * 0.5F, ImGui::GetStyle().WindowPadding.x));
        text_colored(t.text_secondary, title);
    }
    if (hint != nullptr) {
        // Centre each wrapped line (ImGui wraps left-aligned).
        ImGui::Dummy(ImVec2(0.0F, scaled(2.0F)));
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const char* line = hint;
        const char* end = hint + std::strlen(hint);
        while (line < end) {
            const char* brk = font->CalcWordWrapPosition(size, line, end, wrap);
            if (brk == line) brk = line + 1; // a word wider than the line
            const ImVec2 ls = font->CalcTextSizeA(size, FLT_MAX, 0.0F, line, brk);
            ImGui::SetCursorPosX(cx - ls.x * 0.5F);
            ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(t.text_disabled));
            ImGui::TextUnformatted(line, brk);
            ImGui::PopStyleColor();
            line = brk;
            while (line < end && (*line == ' ' || *line == '\n')) ++line;
        }
    }
    bool clicked = false;
    if (action != nullptr) {
        ImGui::Dummy(ImVec2(0.0F, scaled(Spacing::xxs)));
        const float bw = ImGui::CalcTextSize(action).x + ImGui::GetStyle().FramePadding.x * 4.0F;
        ImGui::SetCursorPosX(cx - bw * 0.5F);
        clicked = button(action, ButtonKind::primary, ImVec2(bw, 0.0F));
    }
    return clicked;
}

} // namespace engine::editor::ui
