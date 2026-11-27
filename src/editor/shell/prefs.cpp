#include "prefs.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "engine/core/utf8_path.hpp"

namespace engine::editor::shell {

namespace {

// The whole string must be a finite number (libc++ 18 has no floating-point
// std::from_chars; strtof reports overflow through errno, never UB).
[[nodiscard]] std::optional<float> parse_float(std::string_view s, float lo, float hi) {
    if (s.empty() || s.size() > 64) return std::nullopt;
    // Plain decimal only: strtof would also skip leading spaces and read hex.
    if (s.find_first_not_of("+-.0123456789eE") != std::string_view::npos) return std::nullopt;
    const std::string text(s);
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size() || !std::isfinite(v)) return std::nullopt;
    return std::clamp(v, lo, hi);
}

[[nodiscard]] std::optional<bool> parse_flag(std::string_view s) {
    if (s == "1") return true;
    if (s == "0") return false;
    return std::nullopt;
}

// "Key=value" -> value when `line` starts with `key` and '='.
[[nodiscard]] std::optional<std::string_view> value_of(std::string_view line, std::string_view key) {
    if (line.size() <= key.size() || line.substr(0, key.size()) != key || line[key.size()] != '=') {
        return std::nullopt;
    }
    return line.substr(key.size() + 1);
}

constexpr std::size_t k_max_path_bytes = 1024;

} // namespace

EditorPrefs default_prefs() {
    EditorPrefs p;
    p.panel_open.fill(true);
    return p;
}

const char* scene_path_problem(std::string_view path) {
    if (path.empty()) return "Enter a file name";
    if (path.size() > k_max_path_bytes) return "The path is too long";
    if (path.find('\0') != std::string_view::npos) return "The path contains a NUL byte";
    // Checked before building a path: on Windows the conversion throws on it.
    if (!core::is_valid_utf8(path)) return "The path is not valid UTF-8";
    const std::filesystem::path p = core::path_from_utf8(path);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        return "Use a path relative to the project root";
    }
    for (const std::filesystem::path& part : p) {
        if (part == "..") return "The path must stay inside the project";
    }
    if (p.extension() != ".json") return "Scene files end in .json";
    return nullptr;
}

bool theme_to_save(const ThemePreference& t, bool showing_light) {
    return t.forced ? t.saved_light : showing_light;
}

void push_recent(std::vector<std::string>& recents, std::string path, std::size_t cap) {
    std::erase(recents, path);
    recents.insert(recents.begin(), std::move(path));
    if (recents.size() > cap) recents.resize(cap);
}

bool parse_pref_line(std::string_view line, EditorPrefs& prefs) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (const auto v = value_of(line, "Theme")) {
        if (*v != "light" && *v != "dark") return false;
        prefs.light_theme = *v == "light";
        return true;
    }
    if (const auto v = value_of(line, "Panel")) {
        const std::size_t comma = v->rfind(',');
        if (comma == std::string_view::npos) return false;
        const auto panel = find_panel(v->substr(0, comma));
        const auto open = parse_flag(v->substr(comma + 1));
        if (!panel || !open || !panel_info(*panel).closable) return false;
        prefs.panel_open[static_cast<std::size_t>(*panel)] = *open;
        return true;
    }
    if (const auto v = value_of(line, "SnapEnabled")) {
        const auto f = parse_flag(*v);
        if (!f) return false;
        prefs.snap_enabled = *f;
        return true;
    }
    struct FloatKey {
        std::string_view key;
        float*           out;
        float            lo, hi;
    };
    const FloatKey floats[] = {
        {"SnapMove", &prefs.snap_move, SnapLimits::move_min, SnapLimits::move_max},
        {"SnapRotate", &prefs.snap_rotate_deg, SnapLimits::rotate_min, SnapLimits::rotate_max},
        {"SnapScale", &prefs.snap_scale, SnapLimits::scale_min, SnapLimits::scale_max},
    };
    for (const FloatKey& k : floats) {
        if (const auto v = value_of(line, k.key)) {
            const auto f = parse_float(*v, k.lo, k.hi);
            if (!f) return false;
            *k.out = *f;
            return true;
        }
    }
    if (const auto v = value_of(line, "ViewportStats")) {
        const auto f = parse_flag(*v);
        if (!f) return false;
        prefs.viewport_stats = *f;
        return true;
    }
    if (const auto v = value_of(line, "Scene")) {
        if (scene_path_problem(*v) != nullptr) return false;
        prefs.scene_path = std::string(*v);
        return true;
    }
    if (const auto v = value_of(line, "Recent")) {
        // Lines are written newest first: keep file order, skip duplicates,
        // stop at the cap.
        if (scene_path_problem(*v) != nullptr) return false;
        const std::string path(*v);
        if (prefs.recent_scenes.size() >= k_max_recent_scenes ||
            std::find(prefs.recent_scenes.begin(), prefs.recent_scenes.end(), path) !=
                prefs.recent_scenes.end()) {
            return false;
        }
        prefs.recent_scenes.push_back(path);
        return true;
    }
    return false;
}

std::string write_prefs(const EditorPrefs& prefs) {
    std::string out;
    const auto line = [&](std::string_view key, std::string_view value) {
        out.append(key).append("=").append(value).append("\n");
    };
    const auto number = [](float v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
        return std::string(buf);
    };
    line("Theme", prefs.light_theme ? "light" : "dark");
    for (const PanelInfo& p : panel_infos()) {
        if (!p.closable) continue;
        out.append("Panel=").append(p.title).append(",");
        out.append(prefs.panel_open[static_cast<std::size_t>(p.id)] ? "1" : "0").append("\n");
    }
    line("SnapEnabled", prefs.snap_enabled ? "1" : "0");
    line("SnapMove", number(prefs.snap_move));
    line("SnapRotate", number(prefs.snap_rotate_deg));
    line("SnapScale", number(prefs.snap_scale));
    line("ViewportStats", prefs.viewport_stats ? "1" : "0");
    if (scene_path_problem(prefs.scene_path) == nullptr) line("Scene", prefs.scene_path);
    std::size_t n = 0;
    for (const std::string& r : prefs.recent_scenes) {
        if (n == k_max_recent_scenes) break;
        if (scene_path_problem(r) != nullptr) continue;
        line("Recent", r);
        ++n;
    }
    return out;
}

} // namespace engine::editor::shell
