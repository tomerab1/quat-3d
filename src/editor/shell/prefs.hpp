#pragma once

// Editor preferences as stored in the "[QuatEditor][Prefs]" section of
// <project>/.quat/editor.ini (ROADMAP E8.2). The ini file is external data —
// a user, another tool or a bad merge can put anything in it — so the
// parser is a pure function that validates every value: numbers must parse
// completely, be finite and clamped to the UI's ranges; scene paths
// project-relative, inside the project and ending in .json; recent scenes
// de-duplicated and capped. Unknown or malformed lines are ignored.

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "panels.hpp"

namespace engine::editor::shell {

inline constexpr std::size_t k_max_recent_scenes = 8;

// UI ranges the snap steps are clamped to (also the Preferences sliders').
struct SnapLimits {
    static constexpr float move_min = 0.001F, move_max = 100.0F;
    static constexpr float rotate_min = 0.1F, rotate_max = 180.0F;
    static constexpr float scale_min = 0.001F, scale_max = 10.0F;
};

struct EditorPrefs {
    bool                            light_theme = false;
    std::array<bool, k_panel_count> panel_open{}; // filled with true by default_prefs()
    bool                            snap_enabled = false;
    float                           snap_move = 0.5F;
    float                           snap_rotate_deg = 15.0F;
    float                           snap_scale = 0.1F;
    bool                            viewport_stats = true;
    std::string                     scene_path = "scenes/editor.json";
    std::vector<std::string>        recent_scenes; // newest first
};

[[nodiscard]] EditorPrefs default_prefs();

// Applies one ini line to `prefs`; returns false when the line was ignored
// (unknown key, malformed or rejected value).
bool parse_pref_line(std::string_view line, EditorPrefs& prefs);
// The section's lines (without the "[QuatEditor][Prefs]" header).
[[nodiscard]] std::string write_prefs(const EditorPrefs& prefs);

// Why `path` cannot be a scene file path (empty, too long, NUL, invalid
// UTF-8, absolute, leaves the project, not .json), or null when it can.
[[nodiscard]] const char* scene_path_problem(std::string_view path);

// The theme preference. A theme forced at startup (QUAT_EDITOR_THEME) is
// shown but never saved: the ini keeps the theme it had until the user
// picks one in the editor, which ends the override.
struct ThemePreference {
    bool forced = false;      // a startup override is showing
    bool saved_light = false; // the theme read from the ini (dark by default)
};
// The theme to write back to the ini.
[[nodiscard]] bool theme_to_save(const ThemePreference& t, bool showing_light);

// Moves (or inserts) `path` to the front of `recents`, keeping at most
// `cap` entries. Takes the path by value: callers may pass an element of
// `recents` itself.
void push_recent(std::vector<std::string>& recents, std::string path,
                 std::size_t cap = k_max_recent_scenes);

} // namespace engine::editor::shell
