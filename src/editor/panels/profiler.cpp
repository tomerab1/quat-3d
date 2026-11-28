#include "profiler.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include <imgui.h>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"

namespace engine::editor::panels {

namespace {

struct KindButton {
    ProfilerKind kind;
    const char*  label;
    const char*  tooltip;
};
constexpr std::array<KindButton, 5> k_kinds = {{
    {ProfilerKind::all, "All", "Every zone"},
    {ProfilerKind::pass, "Passes", "Render-graph passes (CPU recording + GPU time)"},
    {ProfilerKind::system, "Systems", "Scene systems run each tick"},
    {ProfilerKind::scope, "Scopes", "Other instrumented scopes"},
    {ProfilerKind::wait, "Waits", "Time blocked on fences, the swapchain or workers"},
}};

constexpr float k_graph_height = 96.0F; // logical px
constexpr int   k_columns = 10;

[[nodiscard]] bool kind_shown(ProfilerKind kind, core::ProfileCategory category) {
    switch (kind) {
    case ProfilerKind::pass: return category == core::ProfileCategory::pass;
    case ProfilerKind::system: return category == core::ProfileCategory::system;
    case ProfilerKind::scope: return category == core::ProfileCategory::scope;
    case ProfilerKind::wait: return category == core::ProfileCategory::wait;
    case ProfilerKind::all: return true;
    }
    return true;
}

[[nodiscard]] ui::Rgba category_color(core::ProfileCategory category) {
    const ui::Theme& t = ui::tokens();
    switch (category) {
    case core::ProfileCategory::pass: return t.accent_text;
    case core::ProfileCategory::system: return t.success;
    case core::ProfileCategory::wait: return t.warning;
    case core::ProfileCategory::scope: return t.text_secondary;
    }
    return t.text_secondary;
}

void toolbar(core::FrameProfiler& profiler, ProfilerState& state) {
    ui::begin_toolbar();
    if (ui::toggle_button("freeze", state.frozen ? icon::play : icon::pause, state.frozen,
                          state.frozen ? "Resume: follow the live statistics"
                                       : "Freeze: keep the current snapshot")) {
        state.frozen = !state.frozen;
    }
    ImGui::SameLine();
    if (ui::icon_button("reset", icon::rotate_ccw, "Reset the profiler's statistics")) {
        profiler.reset();
    }
    ui::toolbar_separator();
    for (const KindButton& k : k_kinds) {
        const bool on = state.kind == k.kind;
        if (ui::button(k.label, on ? ui::ButtonKind::primary : ui::ButtonKind::ghost)) {
            state.kind = k.kind;
        }
        ui::item_tooltip(k.tooltip);
        ImGui::SameLine();
    }
    ui::toolbar_separator();
    ImGui::SetNextItemWidth(ui::scaled(96.0F));
    ImGui::DragFloat("##budget", &state.budget_ms, 0.1F, 1.0F, 100.0F, "budget %.1f ms",
                     ImGuiSliderFlags_AlwaysClamp);
    ui::item_tooltip("Frame budget: the graph's reference line; max values above it are flagged");
    ui::end_toolbar();
    (void)ui::search_field("profiler_filter", state.filter, "Filter zones");
}

[[nodiscard]] std::string stats_text(const core::SampleStats& s) {
    if (s.count == 0) return "-";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%6.2f ms   p95 %6.2f   max %6.2f", static_cast<double>(s.avg),
                  static_cast<double>(s.p95), static_cast<double>(s.max));
    return buf;
}

void summary(const core::FrameProfiler& profiler, const ProfilerState& state) {
    if (!ui::begin_property_grid("profiler_summary")) return;
    const core::FrameSummary& f = state.frame;
    std::string frame = stats_text(f.total);
    if (f.total.count > 0 && f.total.avg > 0.0F) {
        char fps[32];
        std::snprintf(fps, sizeof(fps), "   (%.0f fps)", 1000.0 / static_cast<double>(f.total.avg));
        frame += fps;
    }
    ui::prop_value("Frame", frame, "Wall time between frames", true);
    ui::prop_value("CPU", stats_text(f.cpu), "Frame time minus waits", true);
    ui::prop_value("GPU", stats_text(f.gpu), "First to last GPU timestamp of the frame", true);
    ui::prop_value("Wait", stats_text(f.wait), "Wait-category zones (fences, acquire, workers)",
                   true);
    char frames[32];
    std::snprintf(frames, sizeof(frames), "%u / %u", profiler.frames_in_window(),
                  profiler.window());
    ui::prop_value("Window", frames, "Frames summarised (rolling)", true);
    ui::prop_value("Resolution", state.resolution,
                   "Scene render size -> output size this frame (the upscaler's render scale)",
                   true);
    if (!profiler.gpu_timing_available()) {
        const std::string note = "GPU timing off: " + std::string(profiler.gpu_status());
        ui::prop_note(note.c_str(), ui::tokens().warning);
    }
    ui::end_property_grid();
}

// Wall time and GPU time per frame over the window, with the budget as a
// dashed reference line. Every colour is a theme token.
void frame_graph(const ProfilerState& state) {
    const ui::Theme& t = ui::tokens();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ui::scaled(k_graph_height));
    if (size.x <= 1.0F) return;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##frame_graph", size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->AddRectFilled(origin, end, ui::to_u32(t.bg0), ui::scaled(ui::Radius::md));
    draw->AddRect(origin, end, ui::to_u32(t.border), ui::scaled(ui::Radius::md));

    float top = state.budget_ms * 1.5F;
    for (const float v : state.frame_ms) top = std::max(top, v);
    for (const float v : state.gpu_ms) top = std::max(top, v);
    top *= 1.05F;
    const auto y_of = [&](float ms) { return end.y - (ms / top) * size.y; };

    const float budget_y = std::round(y_of(state.budget_ms));
    const float dash = ui::scaled(ui::Spacing::xs);
    for (float x = origin.x; x < end.x; x += dash) {
        draw->AddLine(ImVec2(x, budget_y), ImVec2(std::min(x + dash * 0.5F, end.x), budget_y),
                      ui::to_u32(t.warning));
    }
    const auto series = [&](const std::vector<float>& values, ui::Rgba colour) {
        if (values.size() < 2) return;
        const float step = size.x / static_cast<float>(values.size() - 1);
        for (std::size_t i = 1; i < values.size(); ++i) {
            draw->AddLine(ImVec2(origin.x + step * static_cast<float>(i - 1), y_of(values[i - 1])),
                          ImVec2(origin.x + step * static_cast<float>(i), y_of(values[i])),
                          ui::to_u32(colour), ui::scaled(1.5F));
        }
    };
    series(state.gpu_ms, t.info);
    series(state.frame_ms, t.accent_text);

    {
        ui::FontScope mono(ui::FontRole::mono);
        char label[32];
        std::snprintf(label, sizeof(label), "%.1f ms", static_cast<double>(top));
        const float pad = ui::scaled(ui::Spacing::xxs);
        draw->AddText(ImVec2(origin.x + pad, origin.y + pad), ui::to_u32(t.text_disabled), label);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip()) {
        ui::text_colored(t.accent_text, "Frame (wall) time");
        ui::text_colored(t.info, "GPU time");
        char budget[64];
        std::snprintf(budget, sizeof(budget), "Dashed: the %.1f ms budget",
                      static_cast<double>(state.budget_ms));
        ui::text_colored(t.warning, budget);
        ui::text_secondary("One point per frame, oldest left");
        ImGui::EndTooltip();
    }
}

void ms_cell(const core::SampleStats& s, float v, float budget_ms) {
    const ui::Theme& t = ui::tokens();
    ImGui::TableNextColumn();
    if (s.count == 0) {
        ui::text_colored(t.text_disabled, "-");
        return;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    ui::text_colored(v > budget_ms ? t.error : t.text, buf);
}

void stats_cells(const core::SampleStats& s, float budget_ms) {
    ms_cell(s, s.avg, budget_ms);
    ms_cell(s, s.p95, budget_ms);
    ms_cell(s, s.max, budget_ms);
}

[[nodiscard]] core::ZoneSort sort_for_column(int column) {
    switch (column) {
    case 0: return core::ZoneSort::name;
    case 2: return core::ZoneSort::self_avg;
    case 3: return core::ZoneSort::cpu_avg;
    case 4: return core::ZoneSort::cpu_p95;
    case 5: return core::ZoneSort::cpu_max;
    case 6: return core::ZoneSort::gpu_avg;
    case 7: return core::ZoneSort::gpu_p95;
    case 8: return core::ZoneSort::gpu_max;
    default: return core::ZoneSort::cost;
    }
}

void zone_table(ProfilerState& state) {
    const bool any = std::any_of(state.zones.begin(), state.zones.end(),
                                 [&](const core::ZoneSummary& z) {
                                     return profiler_row_visible(state, z);
                                 });
    if (!any) {
        (void)ui::empty_state(icon::gauge,
                              state.zones.empty() ? "No zones recorded yet" : "No matching zones",
                              state.zones.empty() ? "Zones appear once frames are profiled"
                                                  : "Change the kind or the filter");
        return;
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##zones", k_columns, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_NoSort);
    // Self: the zone's own CPU time (nested zones excluded); the CPU columns
    // are inclusive.
    ImGui::TableSetupColumn("Self avg", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("CPU avg", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("CPU p95", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("CPU max", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("GPU avg", ImGuiTableColumnFlags_PreferSortDescending |
                                           ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("GPU p95", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("GPU max", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_NoSort);
    ImGui::TableHeadersRow();

    if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        specs != nullptr && specs->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
        core::sort_zones(state.zones, sort_for_column(s.ColumnIndex),
                         s.SortDirection == ImGuiSortDirection_Ascending);
    }

    const ui::Theme& t = ui::tokens();
    // Pushed and popped inside the table: its scrolling child window ends
    // with EndTable, which restores the font stack to its own start.
    ui::push_font(ui::FontRole::mono);
    for (const core::ZoneSummary& z : state.zones) {
        if (!profiler_row_visible(state, z)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(z.name.data(), z.name.data() + z.name.size());
        ImGui::TableNextColumn();
        ui::text_colored(category_color(z.category), core::to_string(z.category));
        ms_cell(z.cpu_self, z.cpu_self.avg, state.budget_ms);
        stats_cells(z.cpu, state.budget_ms);
        stats_cells(z.gpu, state.budget_ms);
        ImGui::TableNextColumn();
        char calls[16];
        std::snprintf(calls, sizeof(calls), "%u", z.calls);
        ui::text_colored(t.text_secondary, calls);
    }
    ui::pop_font();
    ImGui::EndTable();
}

} // namespace

void refresh_profiler(const core::FrameProfiler& profiler, ProfilerState& state) {
    if (state.frozen) return;
    state.zones = profiler.zones();
    profiler.frame_history(state.frame_ms, state.gpu_ms);
    state.frame = profiler.frame_summary();
}

bool profiler_row_visible(const ProfilerState& state, const core::ZoneSummary& zone) {
    return kind_shown(state.kind, zone.category) && ui::matches_filter(zone.name, state.filter);
}

void draw_profiler(core::FrameProfiler* profiler, ProfilerState& state, const char* window_name,
                   bool* open) {
    if (open != nullptr && !*open) return;
    if (!ImGui::Begin(window_name, open)) {
        ImGui::End();
        return;
    }
    if (profiler == nullptr) {
        (void)ui::empty_state(icon::gauge, "No frame profiler",
                              "This build runs without the built-in frame profiler");
        ImGui::End();
        return;
    }
    refresh_profiler(*profiler, state);
    toolbar(*profiler, state);
    summary(*profiler, state);
    frame_graph(state);
    ImGui::Dummy(ImVec2(0.0F, ui::scaled(ui::Spacing::xxs)));
    zone_table(state);
    ImGui::End();
}

} // namespace engine::editor::panels
