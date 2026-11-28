#pragma once

// Console panel (ROADMAP E8.2/E8.3): the engine log ring (A5, engine_core)
// with level filters, a text search, clear, auto-scroll and copy, in the
// monospace font with token level colours. The status bar shows the newest
// line and opens the console on click.
//
// ConsoleModel is the panel's data side (no ImGui): it tails the ring
// incrementally and answers the filter, so it is unit-tested directly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/log.hpp"

namespace engine::editor::panels {

class ConsoleModel {
public:
    static constexpr std::size_t k_max_lines = 5000; // oldest dropped beyond this

    // Appends every line the ring gained since the last poll (bounded per
    // call so a log storm cannot stall a frame). Returns how many arrived.
    std::size_t poll(const core::LogRing& ring, std::size_t max_per_poll = 2000);
    // Forgets the lines received so far (the ring itself is untouched).
    void clear();

    [[nodiscard]] const std::vector<core::LogLine>& lines() const { return lines_; }
    [[nodiscard]] std::uint64_t missed() const { return missed_; }
    // Newest line at or above `min_level` (null when none). Untagged stderr
    // lines (category "stderr": third-party chatter) only count when they
    // are warnings or errors.
    [[nodiscard]] const core::LogLine* last(core::LogLevel min_level = core::LogLevel::info) const;

    // Per-level visibility (trace .. error) and the search text.
    std::array<bool, 5> show{false, false, true, true, true}; // trace/debug hidden by default
    std::string         search;
    [[nodiscard]] bool  visible(const core::LogLine& line) const;
    [[nodiscard]] std::array<std::size_t, 5> counts() const; // lines per level held

private:
    std::vector<core::LogLine> lines_;
    std::uint64_t              cursor_ = 0;
    std::uint64_t              missed_ = 0;
};

struct ConsoleState {
    ConsoleModel model;
    bool         auto_scroll = true;
    bool         scroll_to_bottom = false;
};

// "[12.345] [category] message" as copied to the clipboard.
[[nodiscard]] std::string format_log_line(const core::LogLine& line);

// Draws the Console window (`window_name` from shell::panel_window_name) when
// `*open`; polls the ring every frame regardless, so the status bar is live.
void draw_console(ConsoleState& state, const char* window_name, bool* open);

} // namespace engine::editor::panels
