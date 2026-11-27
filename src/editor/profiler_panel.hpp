#pragma once

// Profiler panel (ROADMAP R4.2): the built-in frame profiler in the editor —
// a frame-time graph (wall and GPU time against a budget line) and a
// sortable, filterable table of passes and systems with CPU and GPU
// avg / p95 / max over the rolling window. Colours come from the ImGui
// style (the editor theme), never literals. Editor-only (src/editor).

#include <array>
#include <vector>

#include "engine/core/frame_profiler.hpp"

namespace engine::editor {

struct ProfilerPanelState {
    bool  open = true;
    bool  frozen = false;   // keep showing the snapshot taken when frozen
    int   kind = 0;         // 0 all, 1 pass, 2 system, 3 scope, 4 wait
    float budget_ms = 16.667F;
    std::array<char, 64> filter{};
    std::vector<core::ZoneSummary> zones; // the table's rows (refreshed unless frozen)
    std::vector<float>             frame_ms;
    std::vector<float>             gpu_ms;
    core::FrameSummary             frame{};
};

// Draws the "Profiler" window when state.open. `profiler` may be null (the
// panel then says so); Reset clears its statistics.
void draw_profiler_panel(core::FrameProfiler* profiler, ProfilerPanelState& state);

} // namespace engine::editor
