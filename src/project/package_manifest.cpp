#include "engine/project/package_manifest.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/project/game_project.hpp"

namespace engine::project {

namespace {

using nlohmann::json;

constexpr std::size_t k_max_files = 1U << 20;
constexpr std::size_t k_max_field = 1024;

constexpr std::array<std::pair<FileKind, std::string_view>, 9> k_kinds{{
    {FileKind::project, "project"},
    {FileKind::scene, "scene"},
    {FileKind::model, "model"},
    {FileKind::model_data, "model_data"},
    {FileKind::audio, "audio"},
    {FileKind::script, "script"},
    {FileKind::icon, "icon"},
    {FileKind::shader, "shader"},
    {FileKind::content, "content"},
}};

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] bool is_sha256_hex(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

} // namespace

std::string_view to_string(PackageConfig config) {
    return config == PackageConfig::shipping ? "shipping" : "development";
}

std::expected<PackageConfig, core::Error> parse_package_config(std::string_view s) {
    if (s == "shipping") return PackageConfig::shipping;
    if (s == "development") return PackageConfig::development;
    return fail("expected \"shipping\" or \"development\" (got '" +
                std::string(s.substr(0, 64)) + "')");
}

std::string_view to_string(FileKind kind) {
    for (const auto& [k, name] : k_kinds) {
        if (k == kind) return name;
    }
    return "content";
}

json manifest_json(const PackageManifest& m) {
    json files = json::array();
    for (const ManifestFile& f : m.files) {
        files.push_back({{"path", f.path},
                         {"bytes", f.bytes},
                         {"sha256", f.sha256},
                         {"kind", std::string(to_string(f.kind))},
                         {"cooked", f.cooked}});
    }
    json out = json::object();
    out["format"] = m.format;
    out["game"] = {{"id", m.game_id}, {"name", m.game_name}, {"version", m.game_version}};
    out["config"] = std::string(to_string(m.config));
    out["target"] = m.target;
    out["build_id"] = m.build_id;
    out["engine"] = m.engine;
    out["files"] = std::move(files);
    return out;
}

std::expected<PackageManifest, core::Error> parse_manifest(const json& doc) {
    if (!doc.is_object()) return fail("manifest.json: expected a JSON object");
    PackageManifest m;
    core::JsonFields f(doc, "");
    f.integer("format", m.format, 1, std::numeric_limits<std::int32_t>::max());
    if (f.ok() && !f.has("format")) f.fail("format", "required");
    if (f.ok() && m.format > k_package_manifest_format) {
        return fail("manifest.json: format " + std::to_string(m.format) +
                    " is newer than this engine reads (" +
                    std::to_string(k_package_manifest_format) + ")");
    }
    std::string config;
    f.string("config", config, 32)
        .string("target", m.target, 64)
        .string("build_id", m.build_id, k_max_field)
        .string("engine", m.engine, k_max_field);
    if (f.ok()) {
        auto c = parse_package_config(config);
        if (c) {
            m.config = *c;
        } else {
            f.fail("config", c.error().message);
        }
    }
    if (const json* game = f.raw("game"); f.ok()) {
        if (game == nullptr) {
            f.fail("game", "required");
        } else {
            core::JsonFields g(*game, "game");
            g.string("id", m.game_id, k_max_field)
                .string("name", m.game_name, k_max_field)
                .string("version", m.game_version, k_max_field);
            if (auto ok = g.finish(); !ok) return fail("manifest.json: " + ok.error().message);
        }
    }
    const json* files = f.raw("files");
    if (auto ok = f.finish(); !ok) return fail("manifest.json: " + ok.error().message);
    if (files == nullptr || !files->is_array() || files->size() > k_max_files) {
        return fail("manifest.json: files: expected an array");
    }
    m.files.reserve(files->size());
    for (std::size_t i = 0; i < files->size(); ++i) {
        const std::string where = "files[" + std::to_string(i) + "]";
        ManifestFile mf;
        std::string kind;
        core::JsonFields e((*files)[i], where);
        e.string("path", mf.path, k_max_field)
            .integer("bytes", mf.bytes, 0, std::numeric_limits<std::int64_t>::max())
            .string("sha256", mf.sha256, 64)
            .string("kind", kind, 32)
            .boolean("cooked", mf.cooked);
        if (e.ok()) {
            if (auto ok = validate_project_path(mf.path); !ok) e.fail("path", ok.error().message);
        }
        if (e.ok() && !is_sha256_hex(mf.sha256)) e.fail("sha256", "expected 64 lower-case hex digits");
        if (e.ok()) {
            const auto it = std::find_if(k_kinds.begin(), k_kinds.end(),
                                         [&](const auto& k) { return k.second == kind; });
            if (it == k_kinds.end()) {
                e.fail("kind", "unknown file kind '" + kind + "'");
            } else {
                mf.kind = it->first;
            }
        }
        if (auto ok = e.finish(); !ok) return fail("manifest.json: " + ok.error().message);
        m.files.push_back(std::move(mf));
    }
    return m;
}

} // namespace engine::project
