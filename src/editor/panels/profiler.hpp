#pragma once

// Profiler panel (ROADMAP R4.2, on the E8 design system): the built-in frame
// profiler in the editor — a summary (frame / CPU / GPU / wait avg, p95, max
// over the rolling window), a frame-time graph (wall and GPU time against a
// budget line) and a sortable, filterable table of passes and systems with
// CPU and GPU avg / p95 / max. Colours come from the ui:: tokens, sizes from
// ui/metrics.hpp, controls from the widget kit (ui/widgets.hpp).
//
// The data side (refresh, row filter) has no ImGui so it is unit-tested
// directly; draw_profiler() is the window.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/frame_profiler.hpp"

namespace engine::editor::panels {

// Which zone categories the table lists.
enum class ProfilerKind : std::uint8_t { all, pass, system, scope, wait };

struct ProfilerState {
    bool         frozen = false; // keep showing the snapshot taken when frozen
    ProfilerKind kind = ProfilerKind::all;
    float        budget_ms = 16.667F; // the graph's reference line; cells over it are flagged
    std::string  filter;              // case-insensitive substring of the zone name
    std::vector<core::ZoneSummary> zones; // the table's rows (refreshed unless frozen)
    std::vector<float>             frame_ms;
    std::vector<float>             gpu_ms;
    core::FrameSummary             frame{};
    std::string                    resolution = "-"; // render -> output (ui::resolution_text)
};

// Copies the profiler's statistics into `state` unless it is frozen.
void refresh_profiler(const core::FrameProfiler& profiler, ProfilerState& state);
// Whether the table shows `zone` under the state's kind and text filters.
[[nodiscard]] bool profiler_row_visible(const ProfilerState& state, const core::ZoneSummary& zone);

// Draws the Profiler window (`window_name` from shell::panel_window_name)
// when `*open` (null: always). `profiler` may be null (the panel then says
// so); Reset clears its statistics. Nothing is refreshed while closed.
void draw_profiler(core::FrameProfiler* profiler, ProfilerState& state, const char* window_name,
                   bool* open);

} // namespace engine::editor::panels
