#include "profiler_panel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string_view>

#include <imgui.h>

namespace engine::editor {

namespace {

constexpr const char* k_kind_labels[] = {"All", "Passes", "Systems", "Scopes", "Waits"};
constexpr float k_graph_height = 90.0F;

[[nodiscard]] bool kind_shown(int kind, core::ProfileCategory category) {
    switch (kind) {
    case 1: return category == core::ProfileCategory::pass;
    case 2: return category == core::ProfileCategory::system;
    case 3: return category == core::ProfileCategory::scope;
    case 4: return category == core::ProfileCategory::wait;
    default: return true;
    }
}

[[nodiscard]] bool contains_ci(std::string_view text, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    return std::tolower(static_cast<unsigned char>(a)) ==
                                           std::tolower(static_cast<unsigned char>(b));
                                });
    return it != text.end();
}

// Wall time and GPU time per frame over the window, with the budget as a
// dashed reference line. Series colours are the style's plot colours.
void draw_frame_graph(const ProfilerPanelState& state) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, k_graph_height);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##frame_graph", size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->AddRectFilled(origin, end, ImGui::GetColorU32(ImGuiCol_FrameBg));

    float top = state.budget_ms * 1.5F;
    for (const float v : state.frame_ms) top = std::max(top, v);
    for (const float v : state.gpu_ms) top = std::max(top, v);
    top *= 1.05F;
    const auto y_of = [&](float ms) { return end.y - (ms / top) * size.y; };

    const float budget_y = y_of(state.budget_ms);
    const ImU32 budget_colour = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (float x = origin.x; x < end.x; x += 8.0F) {
        draw->AddLine(ImVec2(x, budget_y), ImVec2(std::min(x + 4.0F, end.x), budget_y),
                      budget_colour);
    }
    const auto series = [&](const std::vector<float>& values, ImU32 colour) {
        if (values.size() < 2) return;
        const float step = size.x / static_cast<float>(values.size() - 1);
        for (std::size_t i = 1; i < values.size(); ++i) {
            draw->AddLine(ImVec2(origin.x + step * static_cast<float>(i - 1), y_of(values[i - 1])),
                          ImVec2(origin.x + step * static_cast<float>(i), y_of(values[i])),
                          colour, 1.5F);
        }
    };
    series(state.frame_ms, ImGui::GetColorU32(ImGuiCol_PlotLines));
    series(state.gpu_ms, ImGui::GetColorU32(ImGuiCol_PlotHistogram));

    char label[96];
    std::snprintf(label, sizeof(label), "%.1f ms", static_cast<double>(top));
    draw->AddText(ImVec2(origin.x + 4.0F, origin.y + 2.0F),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Frame (wall) time and GPU time per frame, oldest left.\n"
                          "Dashed: the %.1f ms budget.",
                          static_cast<double>(state.budget_ms));
    }
}

void stats_cells(const core::SampleStats& s) {
    for (const float v : {s.avg, s.p95, s.max}) {
        ImGui::TableNextColumn();
        if (s.count == 0) {
            ImGui::TextDisabled("-");
        } else {
            ImGui::Text("%.3f", static_cast<double>(v));
        }
    }
}

[[nodiscard]] core::ZoneSort sort_for_column(int column) {
    switch (column) {
    case 0: return core::ZoneSort::name;
    case 2: return core::ZoneSort::cpu_avg;
    case 3: return core::ZoneSort::cpu_p95;
    case 4: return core::ZoneSort::cpu_max;
    case 5: return core::ZoneSort::gpu_avg;
    case 6: return core::ZoneSort::gpu_p95;
    case 7: return core::ZoneSort::gpu_max;
    default: return core::ZoneSort::cost;
    }
}

void draw_zone_table(ProfilerPanelState& state) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##zones", 9, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_NoSort);
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

    for (const core::ZoneSummary& z : state.zones) {
        if (!kind_shown(state.kind, z.category)) continue;
        if (!contains_ci(z.name, state.filter.data())) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(z.name.data(), z.name.data() + z.name.size());
        ImGui::TableNextColumn();
        const std::string_view kind = core::to_string(z.category);
        ImGui::TextDisabled("%.*s", static_cast<int>(kind.size()), kind.data());
        stats_cells(z.cpu);
        stats_cells(z.gpu);
        ImGui::TableNextColumn();
        ImGui::Text("%u", z.calls);
    }
    ImGui::EndTable();
}

void frame_line(const char* label, const core::SampleStats& s) {
    if (s.count == 0) {
        ImGui::TextDisabled("%s -", label);
        return;
    }
    ImGui::Text("%s %.2f ms (p95 %.2f, max %.2f)", label, static_cast<double>(s.avg),
                static_cast<double>(s.p95), static_cast<double>(s.max));
}

} // namespace

void draw_profiler_panel(core::FrameProfiler* profiler, ProfilerPanelState& state) {
    if (!state.open) return;
    if (!ImGui::Begin("Profiler", &state.open)) {
        ImGui::End();
        return;
    }
    if (profiler == nullptr) {
        ImGui::TextDisabled("No frame profiler in this build.");
        ImGui::End();
        return;
    }
    if (!state.frozen) {
        state.zones = profiler->zones();
        profiler->frame_history(state.frame_ms, state.gpu_ms);
        state.frame = profiler->frame_summary();
    }

    if (ImGui::Button(state.frozen ? "Resume" : "Freeze")) state.frozen = !state.frozen;
    ImGui::SameLine();
    if (ImGui::Button("Reset")) profiler->reset();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    ImGui::DragFloat("budget ms", &state.budget_ms, 0.1F, 1.0F, 100.0F, "%.1f");
    ImGui::SameLine();
    ImGui::TextDisabled("%u frames", profiler->frames_in_window());

    frame_line("Frame", state.frame.total);
    ImGui::SameLine();
    if (state.frame.total.count > 0 && state.frame.total.avg > 0.0F) {
        ImGui::TextDisabled("(%.0f fps)", 1000.0 / static_cast<double>(state.frame.total.avg));
    }
    frame_line("CPU  ", state.frame.cpu);
    frame_line("GPU  ", state.frame.gpu);
    frame_line("Wait ", state.frame.wait);
    if (!profiler->gpu_timing_available()) {
        const std::string_view status = profiler->gpu_status();
        ImGui::TextDisabled("GPU timing off: %.*s", static_cast<int>(status.size()),
                            status.data());
    }

    draw_frame_graph(state);

    ImGui::SetNextItemWidth(110.0F);
    ImGui::Combo("##kind", &state.kind, k_kind_labels, IM_ARRAYSIZE(k_kind_labels));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##filter", "Filter zones", state.filter.data(), state.filter.size());
    draw_zone_table(state);
    ImGui::End();
}

} // namespace engine::editor
