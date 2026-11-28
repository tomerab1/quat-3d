#include "engine/project/game_project.hpp"
#include "engine/version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/core/path_confine.hpp"
#include "engine/core/utf8_path.hpp"

namespace engine::project {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::size_t k_max_name = 128;
constexpr std::size_t k_max_id = 155; // CFBundleIdentifier's practical limit
constexpr std::size_t k_max_version = 64;
constexpr std::size_t k_max_path = 1024;
constexpr int         k_min_window_width = 320;
constexpr int         k_min_window_height = 240;
constexpr int         k_max_window_extent = 16384;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] bool is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

[[nodiscard]] bool has_control_char(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    });
}

[[nodiscard]] bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(s[s.size() - suffix.size() + i]);
        if (std::tolower(a) != static_cast<unsigned char>(suffix[i])) return false;
    }
    return true;
}

// The display name also names the macOS .app folder: no path separators or
// characters Windows forbids in file names, no leading/trailing blanks.
[[nodiscard]] std::expected<void, core::Error> validate_name(std::string_view name) {
    if (name.empty() || name.size() > k_max_name) {
        return fail("expected 1 to " + std::to_string(k_max_name) + " bytes");
    }
    if (has_control_char(name)) return fail("control characters are not allowed");
    if (name.find_first_of("/\\:*?\"<>|") != std::string_view::npos) {
        return fail("the characters / \\ : * ? \" < > | are not allowed (the name becomes a "
                    "folder name)");
    }
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.') {
        return fail("may not start or end with a space or '.'");
    }
    return {};
}

// Number without leading zeros (semver core component).
[[nodiscard]] bool is_semver_number(std::string_view s) {
    if (s.empty() || s.size() > 9) return false;
    if (s.size() > 1 && s.front() == '0') return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Reads game.json's "format" first: a newer file must fail with "update the
// engine", not with whatever unknown field it happens to contain.
[[nodiscard]] std::expected<int, core::Error> read_format(const json& doc) {
    if (!doc.is_object()) return fail("game.json: expected a JSON object");
    const auto it = doc.find("format");
    if (it == doc.end()) {
        return fail("game.json: format: required (this engine reads format " +
                    std::to_string(k_game_project_format) + ")");
    }
    std::int64_t format = 0;
    if (!core::JsonFields::read_int(*it, format) || format < 1) {
        return fail("game.json: format: expected a positive integer");
    }
    if (format > k_game_project_format) {
        return fail("game.json: format " + std::to_string(format) +
                    " is newer than this engine reads (" + std::to_string(k_game_project_format) +
                    "); update the engine");
    }
    return static_cast<int>(format);
}

// Records `what` against `key` when `check` failed.
void check_field(core::JsonFields& f, std::string_view key,
                 const std::expected<void, core::Error>& check) {
    if (!check) f.fail(key, check.error().message);
}

[[nodiscard]] std::expected<void, core::Error> read_window(const json* w, WindowSettings& window) {
    if (w == nullptr || w->is_null()) return {};
    core::JsonFields f(*w, "window");
    f.string("title", window.title, k_max_name)
        .integer("width", window.width, k_min_window_width, k_max_window_extent)
        .integer("height", window.height, k_min_window_height, k_max_window_extent)
        .boolean("fullscreen", window.fullscreen);
    if (f.ok() && has_control_char(window.title)) {
        f.fail("title", "control characters are not allowed");
    }
    return f.finish();
}

void read_content(core::JsonFields& top, std::vector<std::string>& content) {
    const json* c = top.raw("content");
    if (c == nullptr || c->is_null()) return;
    if (!c->is_array() || c->size() > k_max_content_roots) {
        top.fail("content", "expected an array of at most " +
                                std::to_string(k_max_content_roots) +
                                " project-relative paths");
        return;
    }
    for (std::size_t i = 0; i < c->size(); ++i) {
        const std::string key = "content[" + std::to_string(i) + "]";
        const json& v = (*c)[i];
        if (!v.is_string() || v.get_ref<const std::string&>().size() > k_max_path) {
            top.fail(key, "expected a path string");
            return;
        }
        const std::string& path = v.get_ref<const std::string&>();
        if (auto ok = validate_project_path(path); !ok) {
            top.fail(key, ok.error().message);
            return;
        }
        if (std::find(content.begin(), content.end(), path) != content.end()) {
            top.fail(key, "'" + path + "' is listed twice");
            return;
        }
        content.push_back(path);
    }
}

// On-disk check of one project path: confined (symlinks too) and of the
// expected kind.
enum class PathKind : std::uint8_t { file, file_or_directory };
[[nodiscard]] std::expected<void, core::Error> check_on_disk(const fs::path& root,
                                                             std::string_view field,
                                                             const std::string& path,
                                                             PathKind kind) {
    auto confined = core::confine_to_root(root, path);
    if (!confined) {
        return fail("game.json: " + std::string(field) + ": " + confined.error().message);
    }
    std::error_code ec;
    const fs::file_status st = fs::status(*confined, ec);
    const bool ok = kind == PathKind::file ? fs::is_regular_file(st)
                                           : fs::is_regular_file(st) || fs::is_directory(st);
    if (!ok) {
        return fail("game.json: " + std::string(field) + ": no such " +
                    (kind == PathKind::file ? "file" : "file or directory") + " '" + path +
                    "' in the project");
    }
    return {};
}

} // namespace

std::expected<void, core::Error> validate_game_id(std::string_view id) {
    if (id.empty() || id.size() > k_max_id) {
        return fail("expected a reverse-DNS identifier of 1 to " + std::to_string(k_max_id) +
                    " bytes, e.g. \"com.example.my-game\"");
    }
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t dot = id.find('.', start);
        const std::string_view seg =
            id.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (seg.empty()) return fail("empty segment in '" + std::string(id) + "'");
        for (const char c : seg) {
            if (!is_alnum(c) && c != '-') {
                return fail("segments may only contain letters, digits and '-' (got '" +
                            std::string(id) + "')");
            }
        }
        if (segments == 0 && !(seg.front() >= 'a' && seg.front() <= 'z') &&
            !(seg.front() >= 'A' && seg.front() <= 'Z')) {
            return fail("must start with a letter, e.g. \"com.example.my-game\"");
        }
        ++segments;
        if (dot == std::string_view::npos) {
            // The last segment names the executable: no leading '-'.
            if (!is_alnum(seg.front())) {
                return fail("the last segment must start with a letter or digit");
            }
            break;
        }
        start = dot + 1;
    }
    if (segments < 2) {
        return fail("expected at least two dot-separated segments, e.g. \"com.example.my-game\"");
    }
    return {};
}

std::expected<void, core::Error> validate_game_version(std::string_view version) {
    const auto bad = [&] {
        return fail("expected MAJOR.MINOR.PATCH with an optional -prerelease, e.g. \"1.0.0\" or "
                    "\"1.2.0-rc.1\" (got '" +
                    std::string(version.substr(0, k_max_version)) + "')");
    };
    if (version.empty() || version.size() > k_max_version) return bad();
    const std::size_t dash = version.find('-');
    const std::string_view core_part = version.substr(0, dash);
    std::size_t parts = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = core_part.find('.', start);
        const std::string_view n = core_part.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!is_semver_number(n)) return bad();
        ++parts;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (parts != 3) return bad();
    if (dash != std::string_view::npos) {
        const std::string_view pre = version.substr(dash + 1);
        if (pre.empty() || pre.front() == '.' || pre.back() == '.' ||
            pre.find("..") != std::string_view::npos) {
            return bad();
        }
        for (const char c : pre) {
            if (!is_alnum(c) && c != '.' && c != '-') return bad();
        }
    }
    return {};
}

std::expected<void, core::Error> validate_project_path(std::string_view path) {
    if (path.empty()) return fail("expected a project-relative path");
    if (path.size() > k_max_path) {
        return fail("path longer than " + std::to_string(k_max_path) + " bytes");
    }
    if (has_control_char(path)) return fail("control characters are not allowed in paths");
    if (path.find('\\') != std::string_view::npos) {
        return fail("use '/' as the separator in '" + std::string(path) + "'");
    }
    if (path.find(':') != std::string_view::npos) {
        return fail("':' is not allowed in project paths ('" + std::string(path) + "')");
    }
    if (path.front() == '/') {
        return fail("absolute paths are not allowed; give a path relative to the project root");
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string_view seg = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (seg.empty() || seg == "." || seg == "..") {
            return fail("'" + std::string(path) +
                        "' has an empty, '.' or '..' segment; paths stay inside the project");
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return {};
}

// Format 2 (ROADMAP G17.1): modules, mcp, engine. A format 1 file that uses
// them is told which format it needs.
void read_format2(core::JsonFields& f, GameProject& p) {
    for (const char* key : {"modules", "mcp", "engine"}) {
        if (p.format < 2 && f.has(key)) {
            f.fail(key, "needs \"format\": 2");
            return;
        }
    }
    f.known({"modules"});
    if (const json* list = f.raw("modules"); list != nullptr && !list->is_null()) {
        if (!list->is_array() || list->size() > k_max_game_modules) {
            f.fail("modules", "expected an array of at most " + std::to_string(k_max_game_modules) +
                                  " module names");
            return;
        }
        for (const json& m : *list) {
            std::string name;
            const bool ok = core::JsonFields::read_string(m, name) && !name.empty() &&
                            name.size() <= 64 && name.front() >= 'a' && name.front() <= 'z' &&
                            std::ranges::all_of(name, [](char c) {
                                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                            });
            if (!ok) {
                f.fail("modules", "expected module names (lowercase letters, digits and '_', "
                                  "as in quat_add_game_module)");
                return;
            }
            if (std::ranges::find(p.modules, name) != p.modules.end()) {
                f.fail("modules", "'" + name + "' is listed twice");
                return;
            }
            p.modules.push_back(std::move(name));
        }
    }
    f.string("mcp", p.mcp, k_max_path).string("engine", p.engine, 128);
    if (f.ok() && !p.mcp.empty()) check_field(f, "mcp", validate_project_path(p.mcp));
    if (f.ok() && !p.engine.empty()) {
        if (auto ok = validate_engine_range(p.engine); !ok) {
            f.fail("engine", ok.error().message.substr(std::string_view("engine: ").size()));
        }
    }
}

std::expected<GameProject, core::Error> parse_game_project(const json& doc) {
    auto format = read_format(doc);
    if (!format) return std::unexpected(format.error());

    GameProject p;
    p.format = *format;
    core::JsonFields f(doc, "");
    int ignored_format = 0;
    f.integer("format", ignored_format, 1, k_game_project_format);
    f.string("name", p.name, k_max_name)
        .string("id", p.id, k_max_id)
        .string("version", p.version, k_max_version)
        .string("start_scene", p.start_scene, k_max_path)
        .string("icon", p.icon, k_max_path);
    for (const char* required : {"name", "id", "version", "start_scene"}) {
        if (f.ok() && !f.has(required)) f.fail(required, "required");
    }
    if (f.ok()) check_field(f, "name", validate_name(p.name));
    if (f.ok()) check_field(f, "id", validate_game_id(p.id));
    if (f.ok()) check_field(f, "version", validate_game_version(p.version));
    if (f.ok()) {
        check_field(f, "start_scene", validate_project_path(p.start_scene));
        if (f.ok() && !ends_with_ci(p.start_scene, ".json")) {
            f.fail("start_scene", "expected a scene file (.json)");
        }
    }
    if (f.ok() && !p.icon.empty()) {
        check_field(f, "icon", validate_project_path(p.icon));
        if (f.ok() && !ends_with_ci(p.icon, ".png")) f.fail("icon", "expected a .png image");
    }
    const json* window = f.raw("window");
    if (f.ok()) read_content(f, p.content);
    if (f.ok()) read_format2(f, p);
    if (auto ok = f.finish(); !ok) return fail("game.json: " + ok.error().message);
    if (auto ok = read_window(window, p.window); !ok) {
        return fail("game.json: " + ok.error().message);
    }
    return p;
}

std::expected<GameProject, core::Error> load_game_project(const fs::path& project_root) {
    const fs::path file = project_root / fs::path(k_game_project_file);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return fail("no game.json in '" + core::path_to_utf8(project_root) + "'");
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size > k_max_game_project_bytes) {
        return fail("game.json: larger than " + std::to_string(k_max_game_project_bytes) +
                    " bytes");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("game.json: cannot open '" + core::path_to_utf8(file) + "'");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return fail("game.json: not valid JSON");

    auto project = parse_game_project(doc);
    if (!project) return project;
    if (auto ok = check_on_disk(project_root, "start_scene", project->start_scene, PathKind::file);
        !ok) {
        return std::unexpected(ok.error());
    }
    if (!project->icon.empty()) {
        if (auto ok = check_on_disk(project_root, "icon", project->icon, PathKind::file); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (!project->mcp.empty()) {
        if (auto ok = check_on_disk(project_root, "mcp", project->mcp, PathKind::file_or_directory); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (!project->engine.empty()) {
        const quat3d::Version v = quat3d::version();
        if (auto ok = check_engine_range(project->engine, v.major, v.minor, v.patch); !ok) {
            return fail("game.json: " + ok.error().message);
        }
    }
    for (std::size_t i = 0; i < project->content.size(); ++i) {
        if (auto ok = check_on_disk(project_root, "content[" + std::to_string(i) + "]",
                                    project->content[i], PathKind::file_or_directory);
            !ok) {
            return std::unexpected(ok.error());
        }
    }
    return project;
}

json game_project_json(const GameProject& p) {
    json out = json::object();
    out["format"] = p.format;
    out["name"] = p.name;
    out["id"] = p.id;
    out["version"] = p.version;
    out["start_scene"] = p.start_scene;
    out["window"] = {{"title", window_title(p)},
                     {"width", p.window.width},
                     {"height", p.window.height},
                     {"fullscreen", p.window.fullscreen}};
    if (!p.icon.empty()) out["icon"] = p.icon;
    out["content"] = p.content;
    if (!p.modules.empty()) out["modules"] = p.modules;
    if (!p.mcp.empty()) out["mcp"] = p.mcp;
    if (!p.engine.empty()) out["engine"] = p.engine;
    return out;
}

std::expected<void, core::Error> check_output_folder(const GameProject& project,
                                                    const fs::path& project_root,
                                                    const fs::path& output) {
    std::error_code ec;
    const fs::path root = fs::absolute(project_root, ec).lexically_normal();
    const fs::path out =
        fs::absolute(output.is_relative() ? root / output : output, ec).lexically_normal();
    std::string rel = core::path_to_generic_utf8(out.lexically_relative(root));
    while (rel.size() > 1 && rel.back() == '/') rel.pop_back();
    if (rel.empty() || rel == ".." || rel.starts_with("../")) return {}; // outside the project
    if (rel == ".") return fail("output: the project root itself holds the game's files");

    struct Guarded {
        std::string path;
        std::string what;
    };
    std::vector<Guarded> guarded;
    for (const std::string& c : project.content) guarded.push_back({c, "content root"});
    if (const std::size_t slash = project.start_scene.rfind('/'); slash != std::string::npos) {
        guarded.push_back({project.start_scene.substr(0, slash), "start scene's folder"});
    }
    for (const Guarded& g : guarded) {
        const std::string head = "output: '" + rel + "' ";
        const std::string tail = " '" + g.path + "'";
        if (rel == g.path) return fail(head + "is the " + g.what + tail);
        if (rel.starts_with(g.path + "/")) return fail(head + "is inside the " + g.what + tail);
        if (g.path.starts_with(rel + "/")) return fail(head + "contains the " + g.what + tail);
    }
    return {};
}

std::string window_title(const GameProject& project) {
    return project.window.title.empty() ? project.name : project.window.title;
}

std::string executable_slug(const GameProject& project) {
    const std::size_t dot = project.id.rfind('.');
    std::string slug = dot == std::string::npos ? project.id : project.id.substr(dot + 1);
    for (char& c : slug) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return slug;
}

std::string numeric_version(std::string_view version) {
    return std::string(version.substr(0, version.find('-')));
}

} // namespace engine::project
