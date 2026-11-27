#include "engine/rhi/material_shader_registry.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "engine/rhi/material_shader_names.hpp"
#include "engine/rhi/material_shader_source.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::rhi {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

[[nodiscard]] std::unexpected<MaterialShaderRegistry::Error>
err(MaterialShaderRegistry::ErrorKind kind, std::string message) {
    return std::unexpected(MaterialShaderRegistry::Error{kind, std::move(message)});
}

[[nodiscard]] std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Writes via a sibling temporary + rename, so the watcher never compiles a
// half-written file (the temporary's extension is not watched).
[[nodiscard]] bool write_text(const fs::path& path, std::string_view text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

[[nodiscard]] bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// The first line of `text` (a status message without located diagnostics).
[[nodiscard]] std::string first_line(std::string_view text) {
    const std::size_t end = text.find('\n');
    return std::string(text.substr(0, end));
}

} // namespace

std::string_view to_string(MaterialShaderRegistry::ErrorKind kind) {
    switch (kind) {
    case MaterialShaderRegistry::ErrorKind::invalid: return "invalid";
    case MaterialShaderRegistry::ErrorKind::not_found: return "not_found";
    case MaterialShaderRegistry::ErrorKind::exists: return "exists";
    case MaterialShaderRegistry::ErrorKind::unavailable: return "unavailable";
    case MaterialShaderRegistry::ErrorKind::failed: return "failed";
    }
    return "failed";
}

MaterialShaderRegistry::MaterialShaderRegistry(Config config, ShaderHotReload* reload)
    : config_(std::move(config)), reload_(reload) {
    config_.material_dir = config_.material_dir.lexically_normal();
    config_.module_dir = config_.module_dir.lexically_normal();
    config_.cooked_dir = config_.cooked_dir.lexically_normal();
    config_.display_root = config_.display_root.lexically_normal();
}

// ---- Paths ------------------------------------------------------------------

fs::path MaterialShaderRegistry::source_path(std::string_view name) const {
    return config_.material_dir / (std::string(name) + ".slang");
}

std::vector<fs::path> MaterialShaderRegistry::cooked_paths(std::string_view name) const {
    std::vector<fs::path> out;
    for (const material_shaders::Variant& v : material_shaders::variants) {
        const fs::path spv = material_shaders::cooked_spv_path(config_.cooked_dir.string(), name,
                                                               v.name);
        out.push_back(spv);
        out.push_back(fs::path(spv).replace_extension(".json"));
    }
    return out;
}

std::string MaterialShaderRegistry::display(const fs::path& path) const {
    const fs::path norm = path.lexically_normal();
    if (!config_.display_root.empty()) {
        const fs::path rel = norm.lexically_relative(config_.display_root);
        if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
    }
    return norm.generic_string();
}

std::string MaterialShaderRegistry::shorten_paths(std::string text) const {
    if (config_.display_root.empty()) return text;
    std::string prefix = config_.display_root.generic_string();
    if (!prefix.ends_with('/')) prefix += '/';
    for (std::size_t pos = text.find(prefix); pos != std::string::npos;
         pos = text.find(prefix, pos)) {
        text.erase(pos, prefix.size());
    }
    return text;
}

std::vector<ShaderDiagnostic> MaterialShaderRegistry::located(std::string_view output) const {
    std::vector<ShaderDiagnostic> out = parse_slang_diagnostics(output);
    bool unresolved = false;
    for (ShaderDiagnostic& d : out) {
        d.file = shorten_paths(d.file);
        d.message = shorten_paths(d.message);
        unresolved = unresolved || d.message.find("unresolved external symbol") != std::string::npos;
    }
    if (unresolved) {
        ShaderDiagnostic hint;
        hint.severity = "note";
        hint.message = "the module must end with `export struct SurfaceMaterial : IMaterial = "
                       "<your struct>;` and that struct must implement IMaterial";
        out.push_back(std::move(hint));
    }
    return out;
}

// ---- Queries ------------------------------------------------------------------

std::expected<void, MaterialShaderRegistry::Error>
MaterialShaderRegistry::check_name(std::string_view name) const {
    if (!material_shaders::valid_name(name)) {
        return err(ErrorKind::invalid,
                   "name: expected a lowercase identifier (a-z, 0-9, '_', starting with a "
                   "letter, at most " +
                       std::to_string(material_shaders::max_name_length) + " characters)");
    }
    if (name == material_shaders::builtin_name) {
        return err(ErrorKind::invalid, "name: \"" + std::string(name) +
                                           "\" is the built-in material; choose another name");
    }
    if (is_file(config_.module_dir / (std::string(name) + ".slang"))) {
        return err(ErrorKind::invalid, "name: \"" + std::string(name) +
                                           "\" is a shader library module; choose another name");
    }
    return {};
}

std::uint32_t MaterialShaderRegistry::attempts_of(const fs::path& source) const {
    const ShaderStatus* st = reload_ != nullptr ? reload_->status_of(source) : nullptr;
    return st != nullptr ? st->attempts : 0;
}

std::expected<MaterialShaderRegistry::Info, MaterialShaderRegistry::Error>
MaterialShaderRegistry::info(std::string_view name) const {
    Info out;
    out.name = std::string(name);
    if (name == material_shaders::builtin_name) {
        out.builtin = true;
        out.path = display(config_.builtin_path);
        out.state = "builtin";
        return out;
    }
    if (!material_shaders::valid_name(name)) {
        return err(ErrorKind::invalid, "name: not a material shader name");
    }
    const fs::path path = source_path(name);
    if (!is_file(path)) {
        return err(ErrorKind::not_found, "no material shader \"" + out.name + "\" (" +
                                             display(path) + " does not exist)");
    }
    out.path = display(path);

    const auto cooked = cooked_paths(name);
    const bool has_programs = std::all_of(cooked.begin(), cooked.end(), is_file);
    const ShaderStatus* st = reload_ != nullptr ? reload_->status_of(path) : nullptr;
    if (st != nullptr) {
        out.state = std::string(to_string(st->state));
        out.reloads = st->reloads;
        out.message = shorten_paths(st->message);
        out.diagnostics = located(st->message);
    } else {
        out.state = "cooked";
    }
    if (out.state == "cooked" && !has_programs) out.state = "not_compiled";
    if (!ops_.empty() && ops_.front().name == name && ops_.front().phase != Op::Phase::queued) {
        out.state = "compiling";
    }
    return out;
}

std::vector<MaterialShaderRegistry::Info> MaterialShaderRegistry::list() const {
    std::vector<Info> out;
    if (auto builtin = info(material_shaders::builtin_name)) out.push_back(std::move(*builtin));
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(config_.material_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".slang") continue;
        const std::string stem = p.stem().string();
        if (material_shaders::valid_name(stem) && stem != material_shaders::builtin_name) {
            names.push_back(stem);
        }
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (auto i = info(n)) out.push_back(std::move(*i));
    }
    return out;
}

std::expected<std::string, MaterialShaderRegistry::Error>
MaterialShaderRegistry::source(std::string_view name) const {
    auto i = info(name);
    if (!i) return std::unexpected(i.error());
    const fs::path path = i->builtin ? config_.builtin_path : source_path(name);
    auto text = read_text(path);
    if (!text) return err(ErrorKind::failed, "cannot read " + display(path));
    return std::move(*text);
}

std::expected<std::string, MaterialShaderRegistry::Error>
MaterialShaderRegistry::template_source() const {
    auto text = read_text(config_.template_path);
    if (!text) {
        return err(ErrorKind::failed, "cannot read the template " + display(config_.template_path));
    }
    return std::move(*text);
}

// ---- Create / update / remove --------------------------------------------------

void MaterialShaderRegistry::create(std::string name, std::string source, Done done) {
    Op op;
    op.kind = Op::Kind::create;
    op.name = std::move(name);
    op.text = std::move(source);
    op.done = std::move(done);
    ops_.push_back(std::move(op));
    poll(); // starts it now when nothing else is running
}

void MaterialShaderRegistry::update(std::string name, std::string source, Done done) {
    Op op;
    op.kind = Op::Kind::update;
    op.name = std::move(name);
    op.text = std::move(source);
    op.done = std::move(done);
    ops_.push_back(std::move(op));
    poll();
}

std::expected<void, MaterialShaderRegistry::Error>
MaterialShaderRegistry::remove(std::string_view name) {
    if (name == material_shaders::builtin_name) {
        return err(ErrorKind::invalid, "name: the built-in material cannot be deleted");
    }
    if (auto ok = check_name(name); !ok) return ok;
    const fs::path path = source_path(name);
    if (!is_file(path)) {
        return err(ErrorKind::not_found, "no material shader \"" + std::string(name) + "\"");
    }
    for (const Op& op : ops_) {
        if (op.name == name) {
            return err(ErrorKind::failed, "\"" + std::string(name) +
                                              "\" is being compiled; retry when that finishes");
        }
    }
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) return err(ErrorKind::failed, "cannot delete " + display(path) + ": " + ec.message());
    for (const fs::path& p : cooked_paths(name)) fs::remove(p, ec);
    return {};
}

void MaterialShaderRegistry::start(Op& op) {
    const auto fail_now = [&](ErrorKind kind, std::string message) {
        op.done(err(kind, std::move(message)));
        ops_.pop_front();
    };
    if (reload_ == nullptr) {
        fail_now(ErrorKind::unavailable,
                 "material shaders cannot be compiled in this run: shader hot reload is off "
                 "(QUAT_SHADER_HOT_RELOAD=1 enables it)");
        return;
    }
    if (auto ok = check_name(op.name); !ok) return fail_now(ok.error().kind, ok.error().message);
    if (op.text.empty()) return fail_now(ErrorKind::invalid, "source: empty");
    if (op.text.size() > config_.max_source_bytes) {
        return fail_now(ErrorKind::invalid, "source: larger than " +
                                                std::to_string(config_.max_source_bytes) +
                                                " bytes");
    }
    if (op.text.find('\0') != std::string::npos) {
        return fail_now(ErrorKind::invalid, "source: contains a NUL byte");
    }
    // Nothing that makes slangc read files outside the shader library.
    if (auto safe = check_material_source(op.text, library_modules()); !safe) {
        return fail_now(ErrorKind::invalid, "source: " + safe.error().message);
    }

    const fs::path path = source_path(op.name);
    const bool exists = is_file(path);
    if (op.kind == Op::Kind::create && exists) {
        return fail_now(ErrorKind::exists, "material shader \"" + op.name +
                                               "\" already exists; use shader.update to edit it");
    }
    if (op.kind == Op::Kind::update) {
        if (!exists) {
            return fail_now(ErrorKind::not_found, "no material shader \"" + op.name +
                                                      "\"; use shader.create to add it");
        }
        auto previous = read_text(path);
        if (!previous) return fail_now(ErrorKind::failed, "cannot read " + display(path));
        op.previous = std::move(*previous);
    }

    op.attempts_before = attempts_of(path);
    if (!write_text(path, op.text)) {
        return fail_now(ErrorKind::failed, "cannot write " + display(path));
    }
    if (!reload_->request_recompile(path)) {
        std::error_code ec;
        if (op.kind == Op::Kind::create) {
            fs::remove(path, ec);
        } else {
            (void)write_text(path, op.previous);
        }
        return fail_now(ErrorKind::failed,
                        "the shader compiler does not watch " + display(config_.material_dir));
    }
    op.phase = Op::Phase::compiling;
    op.started = Clock::now();
    op.dispatched = false;
}

void MaterialShaderRegistry::finish_compile(Op& op) {
    const fs::path path = source_path(op.name);
    const ShaderStatus* st = reload_->status_of(path);
    Outcome out;
    out.name = op.name;
    out.path = display(path);
    out.ok = st != nullptr && st->state == ShaderStatus::State::ok;
    const std::string message = st != nullptr ? st->message : std::string("no compile result");
    out.log = shorten_paths(message);
    out.diagnostics = located(message);

    // A program must fit its pass's pipeline layout (a material that declares
    // its own resources would not); checked here so the reply says so even
    // before any pipeline uses the material.
    if (out.ok) {
        for (const material_shaders::Variant& v : material_shaders::variants) {
            const fs::path json = fs::path(material_shaders::cooked_spv_path(
                                               config_.cooked_dir.string(), op.name, v.name))
                                      .replace_extension(".json");
            const fs::path base = config_.cooked_dir / (std::string(v.builtin_program) + ".json");
            auto mine_text = read_text(json);
            auto base_text = read_text(base);
            if (!mine_text || !base_text) continue; // nothing to compare against
            auto mine = parse_reflection_json(*mine_text);
            auto baseline = parse_reflection_json(*base_text);
            if (!mine || !baseline) continue;
            static const std::array<std::string, 2> entries{"vertex_main", "fragment_main"};
            if (auto fits = check_reflection_compatible(*baseline, *mine, entries); !fits) {
                out.ok = false;
                ShaderDiagnostic d;
                d.severity = "error";
                d.message = "the material does not fit the " + std::string(v.name) +
                            " pass: " + fits.error().message +
                            " (materials cannot declare their own resources or parameters; "
                            "use MaterialInput and params.shader_params)";
                out.diagnostics.push_back(std::move(d));
            }
        }
    }
    if (!out.ok && std::none_of(out.diagnostics.begin(), out.diagnostics.end(),
                                [](const ShaderDiagnostic& d) { return d.severity == "error"; })) {
        ShaderDiagnostic d;
        d.severity = "error";
        d.message = first_line(out.log.empty() ? std::string("compilation failed") : out.log);
        out.diagnostics.insert(out.diagnostics.begin(), std::move(d));
    }

    if (out.ok) {
        op.done(std::move(out));
        ops_.pop_front();
        return;
    }

    fail_compile(op, std::move(out));
}

void MaterialShaderRegistry::fail_compile(Op& op, Outcome out) {
    // Put the previous version back. The cooked programs were not replaced
    // by a failed compile (a layout misfit did replace them, so the restored
    // source recompiles over them).
    const fs::path path = source_path(op.name);
    std::error_code ec;
    if (op.kind == Op::Kind::create) {
        fs::remove(path, ec);
        for (const fs::path& p : cooked_paths(op.name)) fs::remove(p, ec);
        op.done(std::move(out));
        ops_.pop_front();
        return;
    }
    out.kept_previous = write_text(path, op.previous);
    op.attempts_before = attempts_of(path);
    const bool queued = out.kept_previous && reload_->request_recompile(path);
    op.done(std::move(out));
    if (!queued) {
        ops_.pop_front();
        return;
    }
    op.phase = Op::Phase::restoring;
    op.started = Clock::now();
    op.dispatched = false;
}

void MaterialShaderRegistry::time_out(Op& op) {
    // Exactly like a failed compile: the new source does not stay on disk.
    Outcome out;
    out.name = op.name;
    out.path = display(source_path(op.name));
    ShaderDiagnostic d;
    d.severity = "error";
    d.message = "compiling \"" + op.name + "\" did not finish within " +
                std::to_string(timeout().count() / 1000) + " s";
    out.diagnostics.push_back(std::move(d));
    out.log = out.diagnostics.front().message;
    fail_compile(op, std::move(out));
}

std::chrono::milliseconds MaterialShaderRegistry::timeout() const {
    // Strictly longer than the compiler's own limit for all of a material's
    // slangc runs, so a hung slangc is killed (and reported with its output)
    // before the registry gives up on the operation.
    std::chrono::milliseconds t = config_.compile_timeout;
    if (reload_ != nullptr) {
        const auto runs = static_cast<std::int64_t>(material_shaders::variants.size());
        t = std::max(t, reload_->compiler_config().timeout * runs + std::chrono::seconds(1));
    }
    return t;
}

std::vector<std::string> MaterialShaderRegistry::library_modules() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(config_.module_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == ".slang") out.push_back(it->path().stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void MaterialShaderRegistry::poll() {
    if (polling_) return; // a `done` callback queued more work: this loop takes it
    polling_ = true;
    while (!ops_.empty()) {
        Op& op = ops_.front();
        if (op.phase == Op::Phase::queued) {
            start(op); // pops on immediate failure
            continue;
        }
        const ShaderStatus* st = reload_->status_of(source_path(op.name));
        // The clock runs from when the service picked the compile up (it may
        // have queued behind other work).
        if (!op.dispatched && st != nullptr && st->state == ShaderStatus::State::compiling) {
            op.dispatched = true;
            op.started = Clock::now();
        }
        const bool finished = st != nullptr && st->attempts > op.attempts_before &&
                              st->state != ShaderStatus::State::compiling;
        if (!finished) {
            if (Clock::now() - op.started < timeout()) break; // still running
            if (op.phase == Op::Phase::compiling) {
                time_out(op); // pops, or moves on to restoring
            } else {
                ops_.pop_front(); // a restore that never finished: give up on it
            }
            continue;
        }
        if (op.phase == Op::Phase::compiling) {
            finish_compile(op); // pops, or moves on to restoring
        } else {
            ops_.pop_front(); // the restored version is compiled again
        }
    }
    polling_ = false;
}

} // namespace engine::rhi
