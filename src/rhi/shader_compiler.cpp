#include "engine/rhi/shader_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <utility>

#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/core/child_process.hpp"
#include "engine/rhi/material_shader_names.hpp"

namespace engine::rhi {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] bool is_executable_file(const fs::path& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec);
}

[[nodiscard]] std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// `text` with comments replaced by spaces (string literals kept intact).
[[nodiscard]] std::string strip_comments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '"') { // string literal: copy through the closing quote
            const std::size_t start = i++;
            while (i < text.size() && text[i] != '"' && text[i] != '\n') {
                i += (text[i] == '\\' && i + 1 < text.size()) ? 2 : 1;
            }
            if (i < text.size() && text[i] == '"') ++i;
            out.append(text.substr(start, i - start));
        } else if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') ++i;
            out += ' ';
        } else if (text.compare(i, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
            out += ' ';
        } else {
            out += text[i++];
        }
    }
    return out;
}

[[nodiscard]] bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

[[nodiscard]] fs::path normal(const fs::path& p) { return p.lexically_normal(); }

// Moves the `staged` outputs over `finals` as a set: if any move fails, the
// ones already moved are rolled back from copies kept in `backup_dir`.
[[nodiscard]] std::expected<void, core::Error> install_outputs(std::span<const fs::path> staged,
                                                               std::span<const fs::path> finals,
                                                               const fs::path& backup_dir) {
    // Back up the current outputs (copies: the installed files never go
    // missing), so that if any move below fails the previous set can be put
    // back — the installed .spv and .json always come from one compile.
    struct Backup {
        fs::path saved;
        bool     existed = false;
    };
    std::error_code ec;
    fs::create_directories(backup_dir, ec);
    if (ec) return fail("cannot create " + core::path_to_utf8(backup_dir) + ": " + ec.message());
    std::vector<Backup> backups(finals.size());
    for (std::size_t i = 0; i < finals.size(); ++i) {
        backups[i].saved = backup_dir / finals[i].filename();
        fs::remove(backups[i].saved, ec);
        if (!fs::is_regular_file(finals[i], ec)) continue; // nothing to restore
        fs::copy_file(finals[i], backups[i].saved, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return fail("cannot back up " + core::path_to_utf8(finals[i]) + ": " + ec.message() +
                        " (nothing installed)");
        }
        backups[i].existed = true;
    }
    const auto discard_backups = [&] {
        std::error_code ignored;
        for (const Backup& b : backups) fs::remove(b.saved, ignored);
    };

    // Reflection first, SPIR-V last (the SPIR-V is what consumers key on).
    for (std::size_t i = finals.size(); i-- > 0;) {
        fs::rename(staged[i], finals[i], ec);
        if (!ec) continue;
        std::string what = "cannot install " + core::path_to_utf8(finals[i]) + ": " + ec.message();
        // Roll back the outputs already moved into place.
        bool restored = true;
        for (std::size_t j = i + 1; j < finals.size(); ++j) {
            std::error_code restore_ec;
            if (backups[j].existed) {
                fs::rename(backups[j].saved, finals[j], restore_ec);
            } else {
                fs::remove(finals[j], restore_ec);
            }
            if (restore_ec) {
                restored = false;
                what += "; could not restore " + core::path_to_utf8(finals[j]) + " from " +
                        core::path_to_utf8(backups[j].saved) + ": " + restore_ec.message();
            }
        }
        if (!restored) return fail(what + " (the cooked outputs may not match: rebuild)");
        discard_backups();
        return fail(what + " (previous outputs kept)");
    }
    discard_backups();
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// slangc discovery
// ---------------------------------------------------------------------------

std::optional<fs::path> find_slangc() {
    if (const auto env = core::env_path("QUAT_SLANGC"); env && is_executable_file(*env)) {
        return *env;
    }
#ifdef QUAT_SLANGC_PATH
    if (const fs::path built = core::path_from_utf8(QUAT_SLANGC_PATH); is_executable_file(built)) {
        return built;
    }
#endif
    return core::find_on_path("slangc");
}

// ---------------------------------------------------------------------------
// ShaderCompiler
// ---------------------------------------------------------------------------

std::expected<ShaderCompiler, core::Error> ShaderCompiler::create(ShaderCompilerConfig config) {
    if (config.slangc.empty()) {
        auto found = find_slangc();
        if (!found) {
            return fail("shader compiler: slangc not found (set QUAT_SLANGC or add it to PATH)");
        }
        config.slangc = *found;
    } else if (!is_executable_file(config.slangc)) {
        return fail("shader compiler: slangc not found at " + core::path_to_utf8(config.slangc));
    }
    if (config.module_dir.empty()) config.module_dir = config.source_dir / "lib";
    if (config.material_dir.empty()) {
        config.material_dir = config.source_dir / std::string(material_shaders::subdir);
    }
    config.source_dir = normal(config.source_dir);
    config.module_dir = normal(config.module_dir);
    config.output_dir = normal(config.output_dir);
    config.material_dir = normal(config.material_dir);

    std::error_code ec;
    if (!fs::is_directory(config.source_dir, ec)) {
        return fail("shader compiler: source directory missing: " + core::path_to_utf8(config.source_dir));
    }
    fs::create_directories(config.output_dir, ec);
    if (ec) {
        return fail("shader compiler: cannot create " + core::path_to_utf8(config.output_dir) + ": " +
                    ec.message());
    }
    ShaderCompiler out;
    out.config_ = std::move(config);
    return out;
}

ShaderSourceKind ShaderCompiler::kind_of(const fs::path& source) const {
    const fs::path dir = normal(source).parent_path();
    if (dir == config_.module_dir) return ShaderSourceKind::module;
    if (dir == config_.material_dir) return ShaderSourceKind::material;
    return ShaderSourceKind::pass;
}

std::vector<fs::path> ShaderCompiler::outputs_for(const fs::path& source) const {
    const std::string name = core::path_to_utf8(source.stem());
    switch (kind_of(source)) {
    case ShaderSourceKind::module:
        return {config_.output_dir / (name + ".slang-module")};
    case ShaderSourceKind::material: {
        std::vector<fs::path> out;
        const fs::path dir = config_.output_dir / std::string(material_shaders::subdir);
        for (const material_shaders::Variant& v : material_shaders::variants) {
            const std::string stem = name + "." + std::string(v.name);
            out.push_back(dir / (stem + ".spv"));
            out.push_back(dir / (stem + ".json"));
        }
        return out;
    }
    case ShaderSourceKind::pass: break;
    }
    return {config_.output_dir / (name + ".spv"), config_.output_dir / (name + ".json")};
}

std::vector<std::string> ShaderCompiler::arguments(const fs::path& source,
                                                   const fs::path& out_dir,
                                                   std::size_t variant) const {
    const std::string name = core::path_to_utf8(source.stem());
    // Keep in sync with cmake/CompileShaders.cmake. Search paths come before
    // the inputs: slangc 2025.12+ loads a `.slang-module` input as soon as it
    // parses it, resolving its imports against the -I paths seen so far (#136).
    const ShaderSourceKind kind = kind_of(source);
    if (kind == ShaderSourceKind::module) {
        return {"-I", core::path_to_utf8(config_.module_dir), core::path_to_utf8(source), "-o",
                core::path_to_utf8(out_dir / (name + ".slang-module"))};
    }
    if (kind == ShaderSourceKind::material) {
        const material_shaders::Variant& v =
            material_shaders::variants[std::min(variant, material_shaders::variants.size() - 1)];
        const std::string stem = name + "." + std::string(v.name);
        const fs::path driver =
            config_.output_dir / (std::string(v.driver_module) + ".slang-module");
        return {"-I",
                core::path_to_utf8(config_.module_dir),
                "-I",
                core::path_to_utf8(config_.output_dir),
                core::path_to_utf8(source),
                core::path_to_utf8(driver),
                "-target",
                "spirv",
                "-emit-spirv-directly",
                "-fvk-use-entrypoint-name",
                "-o",
                core::path_to_utf8(out_dir / (stem + ".spv")),
                "-reflection-json",
                core::path_to_utf8(out_dir / (stem + ".json"))};
    }
    return {"-I",
            core::path_to_utf8(config_.module_dir),
            core::path_to_utf8(source),
            "-target",
            "spirv",
            "-emit-spirv-directly",
            "-fvk-use-entrypoint-name",
            "-o",
            core::path_to_utf8(out_dir / (name + ".spv")),
            "-reflection-json",
            core::path_to_utf8(out_dir / (name + ".json"))};
}

ShaderCompileResult ShaderCompiler::compile(const fs::path& source_in,
                                            core::ProcessCanceller* canceller) const {
    ShaderCompileResult result;
    result.source = normal(source_in);
    result.kind = kind_of(result.source);

    // Compile into a staging directory; install only on success so a failed
    // compile leaves the last good outputs (and the running pipelines) intact.
    const fs::path staging = config_.output_dir / ".hot_reload";
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (ec) {
        result.diagnostics = "cannot create staging directory " + core::path_to_utf8(staging) + ": " +
                             ec.message();
        return result;
    }
    std::vector<fs::path> staged;
    for (const fs::path& out : outputs_for(result.source)) {
        staged.push_back(staging / out.filename());
        fs::remove(staged.back(), ec); // stale output from an earlier failed run
    }

    // One slangc run per program (a material has one per pass variant); all
    // must succeed before anything is installed.
    const std::size_t runs =
        result.kind == ShaderSourceKind::material ? material_shaders::variants.size() : 1;
    core::ProcessOptions run_options;
    run_options.timeout = config_.timeout;
    run_options.canceller = canceller;
    bool ok = true;
    for (std::size_t variant = 0; ok && variant < runs; ++variant) {
        const std::vector<std::string> args = arguments(result.source, staging, variant);
        auto run = core::run_process(config_.slangc, args, run_options);
        if (!run) {
            result.diagnostics += run.error().message;
            for (const fs::path& out : staged) fs::remove(out, ec); // partial outputs
            return result;
        }
        if (runs > 1) {
            result.diagnostics +=
                "[" + std::string(material_shaders::variants[variant].name) + "]\n";
        }
        result.diagnostics += run->output;
        if (run->signaled) {
            result.diagnostics += "\nslangc terminated by signal " + std::to_string(run->signal);
        } else if (run->exit_code != 0) {
            result.diagnostics += "\nslangc exited with code " + std::to_string(run->exit_code);
        }
        ok = run->succeeded();
    }

    for (const fs::path& out : staged) {
        if (ok && !fs::is_regular_file(out, ec)) {
            ok = false;
            result.diagnostics += "\nslangc produced no " + core::path_to_utf8(out.filename());
        }
    }
    if (!ok) {
        for (const fs::path& out : staged) fs::remove(out, ec);
        return result;
    }

    std::vector<fs::path> finals = outputs_for(result.source);
    if (!finals.empty()) fs::create_directories(finals.front().parent_path(), ec); // materials/
    if (auto installed = install_outputs(staged, finals, staging / "prev"); !installed) {
        result.diagnostics += "\n" + installed.error().message;
        for (const fs::path& out : staged) fs::remove(out, ec);
        return result;
    }
    result.ok = true;
    result.outputs = std::move(finals);
    return result;
}

// ---------------------------------------------------------------------------
// ShaderDependencyGraph
// ---------------------------------------------------------------------------

ShaderDependencyGraph::ShaderDependencyGraph(fs::path source_dir, fs::path module_dir,
                                             fs::path material_dir)
    : source_dir_(normal(source_dir)), module_dir_(normal(module_dir)),
      material_dir_(material_dir.empty() ? fs::path{} : normal(material_dir)) {}

std::vector<std::string> ShaderDependencyGraph::parse_imports(std::string_view source_text) {
    const std::string text = strip_comments(source_text);
    std::vector<std::string> out;
    constexpr std::string_view keyword = "import";
    for (std::size_t pos = text.find(keyword); pos != std::string::npos;
         pos = text.find(keyword, pos + keyword.size())) {
        // Whole word, at a statement boundary (`__exported import x;` included).
        if (pos > 0 && is_ident_char(text[pos - 1])) continue;
        std::size_t i = pos + keyword.size();
        if (i < text.size() && is_ident_char(text[i])) continue;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
        std::string name;
        if (i < text.size() && text[i] == '"') { // import "path/file.slang";
            const std::size_t end = text.find('"', i + 1);
            if (end == std::string::npos) continue;
            name = text.substr(i + 1, end - i - 1);
            i = end + 1;
            // Only paths inside the shader tree are dependencies: an absolute
            // path or one climbing out with `..` is never tracked (and never
            // resolved to a file to watch or read).
            const fs::path p(name);
            const bool climbs = std::any_of(p.begin(), p.end(),
                                            [](const fs::path& part) { return part == ".."; });
            if (p.is_absolute() || p.has_root_name() || p.has_root_directory() || climbs) {
                continue;
            }
        } else { // import a.b.c;  -> a/b/c
            while (i < text.size() && (is_ident_char(text[i]) || text[i] == '.')) {
                name += text[i] == '.' ? '/' : text[i];
                ++i;
            }
        }
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
        if (name.empty() || i >= text.size() || text[i] != ';') continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    }
    return out;
}

void ShaderDependencyGraph::scan() {
    imports_.clear();
    for (const fs::path& dir : {source_dir_, module_dir_, material_dir_}) {
        if (dir.empty()) continue;
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::path path = dir / it->path().filename();
            if (path.extension() != ".slang") continue;
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) continue;
            update_file(path);
        }
    }
}

void ShaderDependencyGraph::update_file(const fs::path& source) {
    auto text = read_file(source);
    if (!text) {
        remove_file(source);
        return;
    }
    set_imports(source, parse_imports(*text));
}

void ShaderDependencyGraph::remove_file(const fs::path& source) {
    imports_.erase(normal(source));
}

void ShaderDependencyGraph::set_imports(const fs::path& source, std::vector<std::string> imports) {
    imports_[normal(source)] = std::move(imports);
}

bool ShaderDependencyGraph::is_module(const fs::path& source) const {
    return normal(source).parent_path() == module_dir_;
}

bool ShaderDependencyGraph::is_material(const fs::path& source) const {
    return !material_dir_.empty() && normal(source).parent_path() == material_dir_;
}

bool ShaderDependencyGraph::contains(const fs::path& source) const {
    return imports_.contains(normal(source));
}

std::vector<fs::path> ShaderDependencyGraph::sources() const {
    std::vector<fs::path> out;
    out.reserve(imports_.size());
    for (const auto& [path, imports] : imports_) out.push_back(path);
    return out;
}

std::optional<fs::path> ShaderDependencyGraph::resolve(const fs::path& importer,
                                                       const std::string& import) const {
    std::string file = import;
    if (fs::path(file).extension() != ".slang") file += ".slang";
    // Slang searches the importing file's directory, then the -I paths; a
    // module name's underscores may also name a hyphenated file.
    std::string hyphenated = file;
    std::replace(hyphenated.begin(), hyphenated.end(), '_', '-');
    for (const fs::path& dir : {importer.parent_path(), module_dir_}) {
        for (const std::string& name : {file, hyphenated}) {
            const fs::path candidate = normal(dir / name);
            if (imports_.contains(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

std::vector<fs::path> ShaderDependencyGraph::imports_of(const fs::path& source) const {
    std::vector<fs::path> out;
    const fs::path key = normal(source);
    const auto it = imports_.find(key);
    if (it == imports_.end()) return out;
    for (const std::string& name : it->second) {
        if (auto dep = resolve(key, name); dep && *dep != key) out.push_back(*dep);
    }
    // A material is linked against every variant's driver module.
    if (is_material(key)) {
        for (const material_shaders::Variant& v : material_shaders::variants) {
            const fs::path driver = normal(module_dir_ / (std::string(v.driver_module) + ".slang"));
            if (imports_.contains(driver) &&
                std::find(out.begin(), out.end(), driver) == out.end()) {
                out.push_back(driver);
            }
        }
    }
    return out;
}

std::vector<fs::path>
ShaderDependencyGraph::rebuild_order(std::span<const fs::path> changed) const {
    // Reverse edges: imported source -> importers.
    std::map<fs::path, std::vector<fs::path>> importers;
    for (const auto& [path, names] : imports_) {
        for (const fs::path& dep : imports_of(path)) importers[dep].push_back(path);
    }

    // Everything reachable from the changed sources along reverse edges.
    std::set<fs::path> affected;
    std::vector<fs::path> stack;
    for (const fs::path& c : changed) {
        const fs::path key = normal(c);
        if (imports_.contains(key) && affected.insert(key).second) stack.push_back(key);
    }
    while (!stack.empty()) {
        const fs::path cur = std::move(stack.back());
        stack.pop_back();
        const auto it = importers.find(cur);
        if (it == importers.end()) continue;
        for (const fs::path& importer : it->second) {
            if (affected.insert(importer).second) stack.push_back(importer);
        }
    }

    // Depth-first post-order over imports restricted to the affected set: every
    // source is emitted after what it imports (cycles are broken arbitrarily).
    std::vector<fs::path> order;
    std::set<fs::path> visited;
    std::function<void(const fs::path&)> visit = [&](const fs::path& node) {
        if (!visited.insert(node).second) return;
        for (const fs::path& dep : imports_of(node)) {
            if (affected.contains(dep)) visit(dep);
        }
        order.push_back(node);
    };
    for (const fs::path& node : affected) visit(node);

    // Modules first, then passes; stable so dependency order is kept.
    std::stable_partition(order.begin(), order.end(),
                          [&](const fs::path& p) { return is_module(p); });
    return order;
}

} // namespace engine::rhi
