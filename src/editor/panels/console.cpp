#include "console.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/toasts.hpp"
#include "../ui/widgets.hpp"

namespace engine::editor::panels {

namespace {

[[nodiscard]] std::size_t level_index(core::LogLevel l) { return static_cast<std::size_t>(l); }

[[nodiscard]] ui::Rgba level_color(core::LogLevel l) {
    const ui::Theme& t = ui::tokens();
    switch (l) {
    case core::LogLevel::trace: return t.text_disabled;
    case core::LogLevel::debug: return t.text_secondary;
    case core::LogLevel::info: return t.text;
    case core::LogLevel::warn: return t.warning;
    case core::LogLevel::error: return t.error;
    }
    return t.text;
}

} // namespace

// ---- Model -------------------------------------------------------------------

std::size_t ConsoleModel::poll(const core::LogRing& ring, std::size_t max_per_poll) {
    core::LogQuery q;
    q.since_seq = cursor_;
    q.limit = max_per_poll;
    core::LogQueryResult r = ring.query(q);
    cursor_ = r.cursor;
    missed_ += r.missed;
    const std::size_t n = r.lines.size();
    for (core::LogLine& line : r.lines) lines_.push_back(std::move(line));
    if (lines_.size() > k_max_lines) {
        lines_.erase(lines_.begin(),
                     lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - k_max_lines));
    }
    return n;
}

void ConsoleModel::clear() {
    lines_.clear();
    missed_ = 0;
}

const core::LogLine* ConsoleModel::last(core::LogLevel min_level) const {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
        if (it->level < min_level) continue;
        if (it->category == "stderr" && it->level < core::LogLevel::warn) continue;
        return &*it;
    }
    return nullptr;
}

bool ConsoleModel::visible(const core::LogLine& line) const {
    if (!show[level_index(line.level)]) return false;
    if (search.empty()) return true;
    return ui::matches_filter(line.message, search) || ui::matches_filter(line.category, search);
}

std::array<std::size_t, 5> ConsoleModel::counts() const {
    std::array<std::size_t, 5> c{};
    for (const core::LogLine& l : lines_) ++c[level_index(l.level)];
    return c;
}

std::string format_log_line(const core::LogLine& line) {
    char head[48];
    std::snprintf(head, sizeof(head), "[%9.3f] ", line.time_s);
    std::string out = head;
    if (line.level == core::LogLevel::warn || line.level == core::LogLevel::error) {
        out += "[";
        out += core::to_string(line.level);
        out += "] ";
    }
    if (!line.category.empty()) out += "[" + line.category + "] ";
    out += line.message;
    return out;
}

// ---- Panel -------------------------------------------------------------------

void draw_console(ConsoleState& state, const char* window_name, bool* open) {
    ConsoleModel& m = state.model;
    if (m.poll(core::log_ring()) > 0 && state.auto_scroll) state.scroll_to_bottom = true;
    if (open != nullptr && !*open) return;
    if (!ImGui::Begin(window_name, open)) {
        ImGui::End();
        return;
    }
    const ui::Theme& t = ui::tokens();
    const std::array<std::size_t, 5> counts = m.counts();

    // Toolbar: level toggles with counts, search, auto-scroll, copy, clear.
    ui::begin_toolbar();
    struct LevelButton {
        core::LogLevel level;
        const char*    icon;
        const char*    name;
    };
    const LevelButton levels[] = {
        {core::LogLevel::error, icon::circle_x, "Errors"},
        {core::LogLevel::warn, icon::triangle_alert, "Warnings"},
        {core::LogLevel::info, icon::info, "Info"},
        {core::LogLevel::debug, icon::bug, "Debug"},
    };
    for (const LevelButton& b : levels) {
        const std::size_t i = level_index(b.level);
        char text[64];
        std::snprintf(text, sizeof(text), "%s %zu###lvl%zu", ui::ic(b.icon), counts[i], i);
        bool& on = m.show[i];
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ui::to_imvec4(on ? level_color(b.level) : t.text_disabled));
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ui::to_imvec4(on ? t.bg3 : ui::with_alpha(t.bg2, 0.0F)));
        if (ImGui::Button(text)) {
            on = !on;
            if (b.level == core::LogLevel::debug) m.show[level_index(core::LogLevel::trace)] = on;
        }
        ImGui::PopStyleColor(2);
        char tip[64];
        std::snprintf(tip, sizeof(tip), "%s %s", on ? "Hide" : "Show", b.name);
        ui::item_tooltip(tip);
        ImGui::SameLine();
    }
    ui::end_toolbar();
    const float right = ImGui::GetFrameHeight() * 3.0F + ImGui::GetStyle().ItemSpacing.x * 3.0F;
    (void)ui::search_field("console_search", m.search, "Filter log",
                           std::max(ImGui::GetContentRegionAvail().x - right, ui::scaled(80.0F)));
    ImGui::SameLine();
    if (ui::toggle_button("autoscroll", icon::arrow_down, state.auto_scroll,
                          "Auto-scroll to the newest line")) {
        state.auto_scroll = !state.auto_scroll;
        if (state.auto_scroll) state.scroll_to_bottom = true;
    }
    ImGui::SameLine();
    if (ui::icon_button("copy", icon::copy, "Copy the visible lines")) {
        std::string all;
        for (const core::LogLine& l : m.lines()) {
            if (m.visible(l)) all += format_log_line(l) + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
        ui::notify(ui::ToastKind::info, "Log copied to the clipboard", 2.0F);
    }
    ImGui::SameLine();
    if (ui::icon_button("clear", icon::trash_2, "Clear the console")) m.clear();

    // Lines.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::to_imvec4(t.bg0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::scaled(6.0F), ui::scaled(4.0F)));
    ImGui::BeginChild("##log", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);
    std::vector<int> shown;
    shown.reserve(m.lines().size());
    for (std::size_t i = 0; i < m.lines().size(); ++i) {
        if (m.visible(m.lines()[i])) shown.push_back(static_cast<int>(i));
    }
    if (shown.empty()) {
        const bool none = m.lines().empty();
        (void)ui::empty_state(icon::terminal, none ? "No log output yet" : "No matching lines",
                              none ? "Engine and stderr output appears here."
                                   : "Adjust the level toggles or the filter.");
    } else {
        ui::FontScope mono(ui::FontRole::mono);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, ui::scaled(1.0F)));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = static_cast<std::size_t>(shown[static_cast<std::size_t>(row)]);
                const core::LogLine& l = m.lines()[index];
                ImGui::PushID(row);
                char time[24];
                std::snprintf(time, sizeof(time), "%8.2f", l.time_s);
                ui::text_colored(t.text_disabled, time);
                ImGui::SameLine(0.0F, ui::scaled(ui::Spacing::xs));
                if (!l.category.empty()) {
                    ui::text_colored(t.accent_text, l.category);
                    ImGui::SameLine(0.0F, ui::scaled(6.0F));
                }
                ui::text_colored(level_color(l.level), l.message);
                if (ImGui::BeginPopupContextItem("##line")) {
                    if (ImGui::MenuItem(ui::label(icon::copy, "Copy line").c_str())) {
                        ImGui::SetClipboardText(format_log_line(l).c_str());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar();
        // Follow the tail while the user has not scrolled up.
        if (state.scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0F);
            state.scroll_to_bottom = false;
        } else if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.0F &&
                   ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0.0F) {
            state.auto_scroll = false; // scrolled up: stop following
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
}

} // namespace engine::editor::panels
