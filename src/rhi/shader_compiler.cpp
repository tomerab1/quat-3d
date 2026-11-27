#include "engine/rhi/shader_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <utility>

#include "engine/core/child_process.hpp"

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

} // namespace

// ---------------------------------------------------------------------------
// slangc discovery
// ---------------------------------------------------------------------------

std::optional<fs::path> find_slangc() {
    if (const char* env = std::getenv("QUAT_SLANGC"); env != nullptr && *env != '\0') {
        if (is_executable_file(env)) return fs::path(env);
    }
#ifdef QUAT_SLANGC_PATH
    if (is_executable_file(QUAT_SLANGC_PATH)) return fs::path(QUAT_SLANGC_PATH);
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
        return fail("shader compiler: slangc not found at " + config.slangc.string());
    }
    if (config.module_dir.empty()) config.module_dir = config.source_dir / "lib";
    config.source_dir = normal(config.source_dir);
    config.module_dir = normal(config.module_dir);
    config.output_dir = normal(config.output_dir);

    std::error_code ec;
    if (!fs::is_directory(config.source_dir, ec)) {
        return fail("shader compiler: source directory missing: " + config.source_dir.string());
    }
    fs::create_directories(config.output_dir, ec);
    if (ec) {
        return fail("shader compiler: cannot create " + config.output_dir.string() + ": " +
                    ec.message());
    }
    ShaderCompiler out;
    out.config_ = std::move(config);
    return out;
}

ShaderSourceKind ShaderCompiler::kind_of(const fs::path& source) const {
    return normal(source).parent_path() == config_.module_dir ? ShaderSourceKind::module
                                                              : ShaderSourceKind::pass;
}

std::vector<fs::path> ShaderCompiler::outputs_for(const fs::path& source) const {
    const std::string name = source.stem().string();
    if (kind_of(source) == ShaderSourceKind::module) {
        return {config_.output_dir / (name + ".slang-module")};
    }
    return {config_.output_dir / (name + ".spv"), config_.output_dir / (name + ".json")};
}

std::vector<std::string> ShaderCompiler::arguments(const fs::path& source,
                                                   const fs::path& out_dir) const {
    const std::string name = source.stem().string();
    // Keep in sync with cmake/CompileShaders.cmake.
    if (kind_of(source) == ShaderSourceKind::module) {
        return {source.string(), "-I", config_.module_dir.string(), "-o",
                (out_dir / (name + ".slang-module")).string()};
    }
    return {source.string(),
            "-I",
            config_.module_dir.string(),
            "-target",
            "spirv",
            "-emit-spirv-directly",
            "-fvk-use-entrypoint-name",
            "-o",
            (out_dir / (name + ".spv")).string(),
            "-reflection-json",
            (out_dir / (name + ".json")).string()};
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
        result.diagnostics = "cannot create staging directory " + staging.string() + ": " +
                             ec.message();
        return result;
    }
    std::vector<fs::path> staged;
    for (const fs::path& out : outputs_for(result.source)) {
        staged.push_back(staging / out.filename());
        fs::remove(staged.back(), ec); // stale output from an earlier failed run
    }

    const std::vector<std::string> args = arguments(result.source, staging);
    core::ProcessOptions run_options;
    run_options.timeout = config_.timeout;
    run_options.canceller = canceller;
    auto run = core::run_process(config_.slangc, args, run_options);
    if (!run) {
        result.diagnostics = run.error().message;
        for (const fs::path& out : staged) fs::remove(out, ec); // partial outputs
        return result;
    }
    result.diagnostics = std::move(run->output);
    if (run->signaled) {
        result.diagnostics += "\nslangc terminated by signal " + std::to_string(run->signal);
    } else if (run->exit_code != 0) {
        result.diagnostics += "\nslangc exited with code " + std::to_string(run->exit_code);
    }

    bool ok = run->succeeded();
    for (const fs::path& out : staged) {
        if (ok && !fs::is_regular_file(out, ec)) {
            ok = false;
            result.diagnostics += "\nslangc produced no " + out.filename().string();
        }
    }
    if (!ok) {
        for (const fs::path& out : staged) fs::remove(out, ec);
        return result;
    }

    // Install: reflection first, SPIR-V last (the SPIR-V is what consumers key on).
    std::vector<fs::path> finals = outputs_for(result.source);
    for (std::size_t i = finals.size(); i-- > 0;) {
        fs::rename(staged[i], finals[i], ec);
        if (ec) {
            result.diagnostics += "\ncannot install " + finals[i].string() + ": " + ec.message();
            return result;
        }
    }
    result.ok = true;
    result.outputs = std::move(finals);
    return result;
}

// ---------------------------------------------------------------------------
// ShaderDependencyGraph
// ---------------------------------------------------------------------------

ShaderDependencyGraph::ShaderDependencyGraph(fs::path source_dir, fs::path module_dir)
    : source_dir_(normal(source_dir)), module_dir_(normal(module_dir)) {}

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
    for (const fs::path& dir : {source_dir_, module_dir_}) {
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
