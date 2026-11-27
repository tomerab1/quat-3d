#include "engine/pack/stage.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/child_process.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/pack/gltf_refs.hpp"
#include "engine/pack/sha256.hpp"

namespace engine::pack {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using project::FileKind;

constexpr std::chrono::minutes k_cook_timeout{20};
constexpr std::size_t          k_cook_log_tail = 4096;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

struct Entry {
    FileKind kind = FileKind::content;
    bool     cooked = false;
};

class Stager {
public:
    explicit Stager(const StageOptions& o) : o_(o) {}

    std::expected<project::PackageManifest, core::Error> run() {
        std::error_code ec;
        if (fs::exists(o_.data_dir, ec)) {
            return fail("staging directory '" + core::path_to_utf8(o_.data_dir) +
                        "' already exists");
        }
        // assets/ always exists: it marks the data root (core/asset_root.hpp).
        fs::create_directories(o_.data_dir / "assets", ec);
        if (ec) return fail("cannot create '" + core::path_to_utf8(o_.data_dir) + "': " + ec.message());

        if (auto ok = cook_models(); !ok) return std::unexpected(ok.error());
        for (const PackFile& f : o_.files.files) {
            if (auto ok = stage_file(f); !ok) return std::unexpected(ok.error());
        }
        if (auto ok = stage_shaders(); !ok) return std::unexpected(ok.error());
        return write_manifest();
    }

private:
    void log(const std::string& line) const {
        if (o_.log) o_.log(line);
    }

    [[nodiscard]] fs::path src(const std::string& rel) const {
        return o_.project_root / core::path_from_utf8(rel);
    }
    [[nodiscard]] fs::path dst(const std::string& rel) const {
        return o_.data_dir / core::path_from_utf8(rel);
    }

    std::expected<void, core::Error> make_parent(const fs::path& p) const {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec) return fail("cannot create '" + core::path_to_utf8(p.parent_path()) + "': " + ec.message());
        return {};
    }

    std::expected<void, core::Error> copy(const fs::path& from, const fs::path& to) const {
        if (auto ok = make_parent(to); !ok) return ok;
        std::error_code ec;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return fail("cannot copy '" + core::path_to_utf8(from) + "' to '" +
                        core::path_to_utf8(to) + "': " + ec.message());
        }
        return {};
    }

    // Cooks every model when quat-cook is available. The cooked file replaces
    // the original at the same path; its own sidecar files (buffers, KTX2
    // images) come along, and the original's sidecars are no longer shipped
    // unless an uncooked model still needs them.
    std::expected<void, core::Error> cook_models() {
        if (!o_.quat_cook) return {};
        for (const PackFile& f : o_.files.files) {
            if (f.kind != FileKind::model) continue;
            const fs::path out = dst(f.path);
            if (auto ok = make_parent(out); !ok) return ok;
            log("cook " + f.path);
            const std::vector<std::string> args{core::path_to_utf8(src(f.path)),
                                                core::path_to_utf8(out)};
            core::ProcessOptions opts;
            opts.timeout = k_cook_timeout;
            auto r = core::run_process(*o_.quat_cook, std::span<const std::string>(args), opts);
            if (!r || !r->succeeded()) {
                std::string out_text = r ? r->output : r.error().message;
                if (out_text.size() > k_cook_log_tail) {
                    out_text = out_text.substr(out_text.size() - k_cook_log_tail);
                }
                return fail("quat-cook failed on '" + f.path + "':\n" + out_text);
            }
            entries_[f.path] = {FileKind::model, true};
            cooked_.insert(f.path);
            auto uris = gltf_file_uris(out);
            if (!uris) return fail("cooked '" + f.path + "': " + uris.error().message);
            const fs::path dir = core::path_from_utf8(f.path).parent_path();
            for (const std::string& uri : *uris) {
                const std::string rel = core::path_to_generic_utf8(
                    (dir / core::path_from_utf8(uri)).lexically_normal());
                std::error_code ec;
                if (!fs::is_regular_file(dst(rel), ec)) {
                    return fail("cooked '" + f.path + "' references '" + uri +
                                "', which quat-cook did not write");
                }
                entries_[rel] = {FileKind::model_data, true};
            }
        }
        return {};
    }

    std::expected<void, core::Error> stage_file(const PackFile& f) {
        if (f.kind == FileKind::model && cooked_.contains(f.path)) return {};
        if (f.kind == FileKind::model_data && !f.owners.empty() &&
            std::all_of(f.owners.begin(), f.owners.end(),
                        [&](const std::string& m) { return cooked_.contains(m); })) {
            return {}; // superseded by the cooked models' own data
        }
        if (entries_.contains(f.path)) {
            return fail("'" + f.path + "' is both a cooked output and a game file");
        }
        entries_[f.path] = {f.kind, false};
        const auto rewrite = o_.files.scene_rewrites.find(f.path);
        if (rewrite == o_.files.scene_rewrites.end()) return copy(src(f.path), dst(f.path));

        // A scene with absolute in-project paths: ship the relative form.
        std::ifstream in(src(f.path), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) return fail("scene '" + f.path + "': not valid JSON");
        const std::size_t n = rewrite_scene_paths(doc, rewrite->second);
        log("rewrote " + std::to_string(n) + " absolute path(s) in " + f.path);
        if (auto ok = make_parent(dst(f.path)); !ok) return ok;
        std::ofstream out(dst(f.path), std::ios::binary);
        out << doc.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        if (!out.good()) return fail("cannot write '" + core::path_to_utf8(dst(f.path)) + "'");
        return {};
    }

    std::expected<void, core::Error> stage_shaders() {
        std::error_code ec;
        if (!fs::is_directory(o_.shader_dir, ec)) {
            return fail("no cooked shaders at '" + core::path_to_utf8(o_.shader_dir) +
                        "' (build the shaders target, or pass --shaders)");
        }
        std::vector<fs::path> shaders;
        for (const fs::directory_entry& e : fs::directory_iterator(o_.shader_dir, ec)) {
            const fs::path ext = e.path().extension();
            if (e.is_regular_file(ec) && (ext == ".spv" || ext == ".json")) {
                shaders.push_back(e.path());
            }
        }
        if (ec) return fail("cannot list '" + core::path_to_utf8(o_.shader_dir) + "': " + ec.message());
        if (shaders.empty()) {
            return fail("no .spv shaders in '" + core::path_to_utf8(o_.shader_dir) + "'");
        }
        std::sort(shaders.begin(), shaders.end());
        for (const fs::path& s : shaders) {
            const std::string rel = "cooked/shaders/" + core::path_to_utf8(s.filename());
            entries_[rel] = {FileKind::shader, false};
            if (auto ok = copy(s, dst(rel)); !ok) return std::unexpected(ok.error());
        }
        log("shaders: " + std::to_string(shaders.size()) + " files");
        return {};
    }

    std::expected<project::PackageManifest, core::Error> write_manifest() {
        project::PackageManifest m;
        m.game_id = o_.project.id;
        m.game_name = o_.project.name;
        m.game_version = o_.project.version;
        m.config = o_.config;
        m.target = o_.target;
        m.build_id = o_.build_id;
        m.engine = o_.engine_version;
        // Every file in the staged tree (not only the ones recorded): what
        // the manifest describes is what ships.
        std::vector<std::string> paths;
        std::error_code ec;
        fs::recursive_directory_iterator it(o_.data_dir, ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                paths.push_back(core::path_to_generic_utf8(it->path().lexically_relative(o_.data_dir)));
            }
        }
        if (ec) return fail("cannot list the staged data: " + ec.message());
        std::sort(paths.begin(), paths.end());
        for (const std::string& p : paths) {
            if (p == project::k_package_manifest_file) continue;
            project::ManifestFile mf;
            mf.path = p;
            auto digest = sha256_file(dst(p), mf.bytes);
            if (!digest) return std::unexpected(digest.error());
            mf.sha256 = std::move(*digest);
            if (const auto e = entries_.find(p); e != entries_.end()) {
                mf.kind = e->second.kind;
                mf.cooked = e->second.cooked;
            }
            m.files.push_back(std::move(mf));
        }
        std::ofstream out(dst(std::string(project::k_package_manifest_file)), std::ios::binary);
        out << project::manifest_json(m).dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        if (!out.good()) return fail("cannot write manifest.json");
        return m;
    }

    const StageOptions&         o_;
    std::map<std::string, Entry> entries_;
    std::set<std::string>       cooked_;
};

} // namespace

std::expected<project::PackageManifest, core::Error> stage_game_data(const StageOptions& options) {
    return Stager(options).run();
}

} // namespace engine::pack
