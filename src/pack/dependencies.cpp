#include "engine/pack/dependencies.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/path_confine.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/pack/gltf_refs.hpp"
#include "engine/pack/lua_refs.hpp"
#include "engine/scene/scene_references.hpp"

namespace engine::pack {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using project::FileKind;

constexpr std::uintmax_t k_max_script_bytes = 16ULL << 20;
constexpr std::uintmax_t k_max_json_probe_bytes = 256ULL << 20;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

[[nodiscard]] std::string extension(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return lower(path.substr(dot));
}

[[nodiscard]] bool is_hidden(const fs::path& p) {
    const std::string name = core::path_to_utf8(p.filename());
    return !name.empty() && name.front() == '.';
}

[[nodiscard]] bool read_text(const fs::path& path, std::uintmax_t max, std::string& out) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > max) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The work list: files to scan once each, in deterministic order.
enum class Scan : std::uint8_t { scene, model, script };

class Collector {
public:
    Collector(fs::path root) : root_(std::move(root)) {}

    std::expected<GameFiles, core::Error> run(const project::GameProject& project) {
        native_modules_ = project.modules;
        add(std::string(project::k_game_project_file), FileKind::project, "the game project");
        if (!project.icon.empty()) add(project.icon, FileKind::icon, "game.json icon");
        if (auto ok = add_existing(project.start_scene, "game.json start_scene", FileKind::scene);
            !ok) {
            return std::unexpected(ok.error());
        }
        for (const std::string& c : project.content) {
            if (auto ok = add_content_root(c); !ok) return std::unexpected(ok.error());
        }
        while (!queue_.empty()) {
            const auto [scan, path] = queue_.front();
            queue_.pop_front();
            std::expected<void, core::Error> ok;
            switch (scan) {
            case Scan::scene: ok = scan_scene(path); break;
            case Scan::model: ok = scan_model(path); break;
            case Scan::script: ok = scan_script(path); break;
            }
            if (!ok) return std::unexpected(ok.error());
        }
        GameFiles out;
        for (auto& [path, file] : files_) {
            std::sort(file.owners.begin(), file.owners.end());
            file.owners.erase(std::unique(file.owners.begin(), file.owners.end()), file.owners.end());
            out.files.push_back(std::move(file));
        }
        out.scene_rewrites = std::move(rewrites_);
        out.warnings = std::move(warnings_);
        return out;
    }

private:
    // Registers a file (first reason wins) and queues it for scanning.
    void add(const std::string& path, FileKind kind, const std::string& reason) {
        auto [it, inserted] = files_.try_emplace(path);
        if (inserted) {
            it->second.path = path;
            it->second.kind = kind;
            it->second.reason = reason;
        } else if (it->second.kind == FileKind::content && kind != FileKind::content) {
            it->second.kind = kind; // a content-root file someone references
        }
        const auto queue = [&](Scan s) {
            if (scanned_.insert(path).second) queue_.push_back({s, path});
        };
        if (kind == FileKind::scene) queue(Scan::scene);
        if (kind == FileKind::model) queue(Scan::model);
        if (kind == FileKind::script) queue(Scan::script);
    }

    // A project-relative path that must exist as a file.
    std::expected<void, core::Error> add_existing(const std::string& path, const std::string& who,
                                                  FileKind kind) {
        auto abs = core::confine_to_root(root_, path);
        if (!abs) return fail(who + ": '" + path + "': " + abs.error().message);
        std::error_code ec;
        if (!fs::is_regular_file(*abs, ec)) return fail(who + ": no such file '" + path + "'");
        add(core::path_to_generic_utf8(core::path_from_utf8(path).lexically_normal()), kind, who);
        return {};
    }

    std::expected<void, core::Error> add_content_root(const std::string& entry) {
        const std::string who = "game.json content '" + entry + "'";
        auto abs = core::confine_to_root(root_, entry);
        if (!abs) return fail(who + ": " + abs.error().message);
        std::error_code ec;
        if (fs::is_regular_file(*abs, ec)) return add_content_file(entry, who);
        if (!fs::is_directory(*abs, ec)) return fail(who + ": no such file or directory");
        std::vector<std::string> found;
        fs::recursive_directory_iterator it(*abs, fs::directory_options::none, ec);
        if (ec) return fail(who + ": cannot list (" + ec.message() + ")");
        for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
            if (ec) return fail(who + ": cannot list (" + ec.message() + ")");
            if (is_hidden(it->path())) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            found.push_back(core::path_to_generic_utf8(it->path().lexically_relative(root_)));
        }
        if (ec) return fail(who + ": cannot list (" + ec.message() + ")");
        std::sort(found.begin(), found.end());
        for (const std::string& f : found) {
            if (auto ok = add_content_file(f, who); !ok) return ok;
        }
        return {};
    }

    std::expected<void, core::Error> add_content_file(const std::string& path,
                                                      const std::string& who) {
        // Confined one by one: a symlink inside the root may point outside.
        auto abs = core::confine_to_root(root_, path);
        if (!abs) return fail(who + ": '" + path + "': " + abs.error().message);
        FileKind kind = kind_for_path(path);
        if (extension(path) == ".json") {
            std::string text;
            if (read_text(*abs, k_max_json_probe_bytes, text) &&
                scene::is_scene_document(json::parse(text, nullptr, false))) {
                kind = FileKind::scene;
            }
        }
        add(path, kind, who);
        return {};
    }

    // Resolves a path written in a scene (project-relative or absolute) to a
    // project-relative one, recording a rewrite for absolute paths inside the
    // project.
    std::expected<std::string, core::Error> resolve_scene_path(const std::string& scene,
                                                               const std::string& written,
                                                               const std::string& where) {
        const fs::path p = core::path_from_utf8(written);
        const std::string ctx = "scene '" + scene + "' " + where;
        if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
            const fs::path rel = core::relative_inside(root_, p);
            if (rel.empty()) {
                return fail(ctx + ": '" + written +
                            "' is outside the game project; copy the file into the project "
                            "and reference it by a project-relative path");
            }
            const std::string to = core::path_to_generic_utf8(rel);
            auto& list = rewrites_[scene];
            if (std::none_of(list.begin(), list.end(),
                             [&](const PathRewrite& r) { return r.from == written; })) {
                list.push_back({written, to});
            }
            return check_file(ctx, to);
        }
        return check_file(ctx, core::path_to_generic_utf8(p.lexically_normal()));
    }

    std::expected<std::string, core::Error> check_file(const std::string& ctx,
                                                       const std::string& rel) {
        auto abs = core::confine_to_root(root_, rel);
        if (!abs) return fail(ctx + ": '" + rel + "': " + abs.error().message);
        std::error_code ec;
        if (!fs::is_regular_file(*abs, ec)) return fail(ctx + ": no such file '" + rel + "'");
        return core::path_to_generic_utf8(core::path_from_utf8(rel).lexically_normal());
    }

    std::expected<void, core::Error> scan_scene(const std::string& scene) {
        auto refs = scene::scene_file_references(root_ / core::path_from_utf8(scene));
        if (!refs) return std::unexpected(refs.error());
        for (const scene::SceneReference& ref : *refs) {
            if (ref.kind == scene::SceneRefKind::script) {
                if (auto ok = project::validate_project_path(ref.path); !ok) {
                    return fail("scene '" + scene + "' " + ref.where + ": " + ok.error().message);
                }
                auto path = check_file("scene '" + scene + "' " + ref.where,
                                       std::string(k_scripts_dir) + "/" + ref.path);
                if (!path) return std::unexpected(path.error());
                add(*path, FileKind::script, "scene '" + scene + "' " + ref.where);
                continue;
            }
            auto path = resolve_scene_path(scene, ref.path, ref.where);
            if (!path) return std::unexpected(path.error());
            const FileKind kind =
                ref.kind == scene::SceneRefKind::model ? FileKind::model : FileKind::audio;
            add(*path, kind, "scene '" + scene + "' " + ref.where);
        }
        return {};
    }

    std::expected<void, core::Error> scan_model(const std::string& model) {
        auto uris = gltf_file_uris(root_ / core::path_from_utf8(model));
        if (!uris) return std::unexpected(uris.error());
        const fs::path dir = core::path_from_utf8(model).parent_path();
        for (const std::string& uri : *uris) {
            const fs::path u = core::path_from_utf8(uri);
            if (u.is_absolute() || u.has_root_name() || u.has_root_directory()) {
                return fail("model '" + model + "': '" + uri +
                            "' is absolute; glTF files may only reference files next to them");
            }
            const std::string rel = core::path_to_generic_utf8((dir / u).lexically_normal());
            auto path = check_file("model '" + model + "'", rel);
            if (!path) return std::unexpected(path.error());
            add(*path, FileKind::model_data, "model '" + model + "'");
            files_[*path].owners.push_back(model);
        }
        return {};
    }

    std::expected<void, core::Error> scan_script(const std::string& script) {
        std::string text;
        if (!read_text(root_ / core::path_from_utf8(script), k_max_script_bytes, text)) {
            return fail("script '" + script + "': cannot read");
        }
        for (const LuaLiteral& lit : lua_string_literals(text)) {
            const std::string where = "script '" + script + "' line " + std::to_string(lit.line);
            if (lit.required) {
                // A native module's Lua module (G17.1) shares the module's
                // name and has no file: require("coin_collector").
                if (std::ranges::find(native_modules_, lit.text) != native_modules_.end()) continue;
                std::string module = lit.text;
                if (module.ends_with(".lua")) module.resize(module.size() - 4);
                std::replace(module.begin(), module.end(), '.', '/');
                const std::string path = std::string(k_scripts_dir) + "/" + module + ".lua";
                if (!try_add(path, FileKind::script, where)) {
                    warnings_.push_back(where + ": require('" + lit.text + "'): no " + path +
                                        " in the project");
                }
                continue;
            }
            if (!project::validate_project_path(lit.text)) continue; // not a path
            if (lit.text.starts_with("assets/")) {
                const FileKind kind = kind_for_path(lit.text);
                if (kind == FileKind::content && extension(lit.text).empty()) continue;
                if (!try_add(lit.text, kind == FileKind::content && extension(lit.text) == ".json"
                                               ? FileKind::scene
                                               : kind,
                             where)) {
                    warnings_.push_back(where + ": '" + lit.text +
                                        "' looks like a project file but does not exist");
                }
            } else if (extension(lit.text) == ".lua") {
                const std::string path = std::string(k_scripts_dir) + "/" + lit.text;
                if (!try_add(path, FileKind::script, where)) {
                    warnings_.push_back(where + ": '" + lit.text + "' looks like a script but " +
                                        path + " does not exist");
                }
            }
        }
        return {};
    }

    // Adds `path` when it is an existing file inside the project.
    bool try_add(const std::string& path, FileKind kind, const std::string& who) {
        if (!project::validate_project_path(path)) return false;
        auto abs = core::confine_to_root(root_, path);
        std::error_code ec;
        if (!abs || !fs::is_regular_file(*abs, ec)) return false;
        if (kind == FileKind::scene) {
            std::string text;
            if (!read_text(*abs, k_max_json_probe_bytes, text) ||
                !scene::is_scene_document(json::parse(text, nullptr, false))) {
                kind = FileKind::content;
            }
        }
        add(path, kind, who);
        return true;
    }

    fs::path                                         root_;
    std::map<std::string, PackFile>                  files_;
    std::set<std::string>                            scanned_;
    std::deque<std::pair<Scan, std::string>>         queue_;
    std::map<std::string, std::vector<PathRewrite>>  rewrites_;
    std::vector<std::string>                         warnings_;
    std::vector<std::string>                         native_modules_; // game.json "modules"
};

void rewrite_string(json& value, std::span<const PathRewrite> rewrites, std::size_t& changed) {
    if (!value.is_string()) return;
    const std::string& s = value.get_ref<const std::string&>();
    for (const PathRewrite& r : rewrites) {
        if (s == r.from) {
            value = r.to;
            ++changed;
            return;
        }
        if (s.size() > r.from.size() && s.starts_with(r.from) && s[r.from.size()] == '#') {
            value = r.to + s.substr(r.from.size());
            ++changed;
            return;
        }
    }
}

} // namespace

FileKind kind_for_path(std::string_view path) {
    const std::string ext = extension(path);
    if (ext == ".gltf" || ext == ".glb") return FileKind::model;
    if (ext == ".lua") return FileKind::script;
    if (ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".ogg") return FileKind::audio;
    // Images and buffers are model_data once a model references them.
    return FileKind::content;
}

std::expected<GameFiles, core::Error> collect_game_files(const fs::path& project_root,
                                                         const project::GameProject& project) {
    return Collector(project_root).run(project);
}

std::size_t rewrite_scene_paths(json& scene, std::span<const PathRewrite> rewrites) {
    std::size_t changed = 0;
    if (!scene::is_scene_document(scene) || rewrites.empty()) return 0;
    for (json& entity : scene["entities"]) {
        if (!entity.is_object()) continue;
        const auto field = [&](const char* component, const char* key) {
            const auto c = entity.find(component);
            if (c == entity.end() || !c->is_object()) return;
            const auto v = c->find(key);
            if (v != c->end()) rewrite_string(*v, rewrites, changed);
        };
        field("mesh", "gltf");
        field("mesh", "mesh_key");
        field("mesh", "material_key");
        field("instanced_mesh", "gltf");
        field("audio_source", "clip");
    }
    return changed;
}

} // namespace engine::pack
