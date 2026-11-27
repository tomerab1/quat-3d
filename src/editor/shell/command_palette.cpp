// Command palette (ROADMAP E8.2): Ctrl+P / Ctrl+Shift+P opens a fuzzy search
// over every registered command — menu actions, panel toggles, tools and the
// Create entries. Up / Down select, Enter runs, Esc closes.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"
#include "fuzzy.hpp"

namespace engine::editor::shell {

namespace {

constexpr int k_visible_rows = 12;

// Commands in palette order for `query`: recent first when empty, else by
// fuzzy score over "label category".
std::vector<const Command*> ranked(const CommandRegistry& reg, std::string_view query) {
    std::vector<const Command*> out;
    if (query.empty()) {
        for (const std::string& id : reg.recent()) {
            if (const Command* c = reg.find(id)) out.push_back(c);
        }
        for (const Command& c : reg.all()) {
            if (std::find(out.begin(), out.end(), &c) == out.end()) out.push_back(&c);
        }
        return out;
    }
    std::vector<std::string>      texts;
    std::vector<std::string_view> views;
    texts.reserve(reg.all().size());
    for (const Command& c : reg.all()) texts.push_back(c.label + " " + c.category);
    for (const std::string& t : texts) views.emplace_back(t);
    for (const std::size_t i : fuzzy_rank(query, views)) out.push_back(&reg.all()[i]);
    return out;
}

} // namespace

void draw_command_palette(EditorShell& shell) {
    const ui::Theme& t = ui::tokens();
    if (shell.palette_open && !ImGui::IsPopupOpen("##command_palette")) {
        ImGui::OpenPopup("##command_palette");
        shell.palette = {};
        shell.palette.focus_input = true;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width =
        std::min(ui::scaled(ui::Sizes::palette_width), vp->WorkSize.x - ui::scaled(32.0F));
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + ui::scaled(48.0F)),
        ImGuiCond_Always, ImVec2(0.5F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));
    const float pad = ui::scaled(ui::Spacing::xs);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    const bool open = ImGui::BeginPopup("##command_palette", ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    if (!open) {
        shell.palette_open = false;
        return;
    }

    if (shell.palette.focus_input) {
        ImGui::SetKeyboardFocusHere();
        shell.palette.focus_input = false;
    }
    if (ui::search_field("palette_query", shell.palette.query, "Type a command (fuzzy)")) {
        shell.palette.cursor = 0;
    }
    const std::vector<const Command*> items = ranked(shell.commands, shell.palette.query);
    const int count = static_cast<int>(items.size());
    bool keyboard_moved = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        shell.palette.cursor = std::min(shell.palette.cursor + 1, count - 1);
        keyboard_moved = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        shell.palette.cursor = std::max(shell.palette.cursor - 1, 0);
        keyboard_moved = true;
    }
    shell.palette.cursor = std::clamp(shell.palette.cursor, 0, std::max(count - 1, 0));

    std::string run_id;
    const float row_h = ImGui::GetFrameHeight();
    const float list_h = row_h * static_cast<float>(std::min(std::max(count, 1), k_visible_rows));
    ImGui::BeginChild("##palette_list", ImVec2(0.0F, list_h + ui::scaled(4.0F)));
    if (count == 0) {
        ImGui::AlignTextToFramePadding();
        ui::text_colored(t.text_disabled, "No matching commands");
    }
    for (int i = 0; i < count; ++i) {
        const Command& c = *items[static_cast<std::size_t>(i)];
        const bool enabled = shell.commands.is_enabled(c);
        ImGui::PushID(c.id.c_str());
        const bool is_cursor = i == shell.palette.cursor;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##row", is_cursor, ImGuiSelectableFlags_None, ImVec2(w, row_h))) {
            if (enabled) run_id = c.id;
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x != 0.0F) shell.palette.cursor = i;
        if (is_cursor && keyboard_moved) ImGui::SetScrollHereY(0.5F);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ty = p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5F;
        const ui::Rgba fg = enabled ? t.text : t.text_disabled;
        float x = p.x + ui::scaled(ui::Spacing::xs);
        const char* glyph = ui::ic(c.icon != nullptr ? c.icon : icon::command);
        dl->AddText(ImVec2(x, ty), ui::to_u32(enabled ? t.accent_text : t.text_disabled), glyph);
        x += ImGui::GetFontSize() + ui::scaled(ui::Spacing::xs);
        dl->AddText(ImVec2(x, ty), ui::to_u32(fg), c.label.c_str());
        x += ImGui::CalcTextSize(c.label.c_str()).x + ui::scaled(ui::Spacing::xs);
        dl->AddText(ImVec2(x, ty), ui::to_u32(t.text_disabled), c.category.c_str());
        if (shell.commands.is_checked(c)) {
            x += ImGui::CalcTextSize(c.category.c_str()).x + ui::scaled(ui::Spacing::xs);
            dl->AddText(ImVec2(x, ty), ui::to_u32(t.accent_text), ui::ic(icon::check));
        }
        const std::string keys = format_chord(c.shortcut);
        if (!keys.empty()) {
            const float kw = ImGui::CalcTextSize(keys.c_str()).x;
            dl->AddText(ImVec2(p.x + w - kw - ui::scaled(ui::Spacing::xs), ty),
                        ui::to_u32(t.text_secondary), keys.c_str());
        }
        if (!c.hint.empty()) ui::item_tooltip(c.hint.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Footer: how to drive it.
    ui::text_colored(t.text_disabled, "Up/Down to select  -  Enter to run  -  Esc to close");

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (count > 0) {
            const Command& c = *items[static_cast<std::size_t>(shell.palette.cursor)];
            if (shell.commands.is_enabled(c)) run_id = c.id;
        }
    }
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (!run_id.empty() || escape) {
        ImGui::CloseCurrentPopup();
        shell.palette_open = false;
    }
    ImGui::EndPopup();
    // Run after the popup closed so commands that open their own popups
    // (Open Scene, Preferences) are not nested inside the palette.
    if (!run_id.empty()) (void)shell.commands.run(run_id);
}

} // namespace engine::editor::shell
