#pragma once

// Shader compiler service (ROADMAP R2: shader hot reload).
//
// ShaderCompiler runs slangc as a child process with exactly the flags
// cmake/CompileShaders.cmake uses:
//   * library modules (module_dir/*.slang, no entry points):
//       slangc <src> -I <module_dir> -o <out>/<name>.slang-module
//   * pass shaders (source_dir/*.slang):
//       slangc <src> -I <module_dir> -target spirv -emit-spirv-directly
//              -fvk-use-entrypoint-name -o <out>/<name>.spv
//              -reflection-json <out>/<name>.json
// It compiles into a staging directory under the output directory and only
// moves the outputs into place when slangc succeeds, so a failed compile never
// clobbers the last good cooked shader. The captured stdout/stderr is returned
// as the diagnostics (warnings on success, errors on failure).
//
// ShaderDependencyGraph tracks `import` declarations between Slang sources so a
// library-module edit recompiles the modules and passes that import it.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::core {
class ProcessCanceller;
} // namespace engine::core

namespace engine::rhi {

enum class ShaderSourceKind : std::uint8_t {
    pass,   // has entry points: compiled to .spv + reflection .json
    module, // library module: compiled to .slang-module
};

struct ShaderCompilerConfig {
    std::filesystem::path slangc;     // empty: find_slangc()
    std::filesystem::path source_dir; // assets/shaders (pass shaders)
    std::filesystem::path module_dir; // empty: source_dir / "lib"
    std::filesystem::path output_dir; // cooked/shaders
    // A slangc run taking longer is killed and reported as a failed compile
    // (a hung compiler must not stall every later reload).
    std::chrono::milliseconds timeout{60'000};
};

struct ShaderCompileResult {
    std::filesystem::path source;
    ShaderSourceKind      kind = ShaderSourceKind::pass;
    bool                  ok   = false;
    std::string           diagnostics; // slangc output (+ a note when it crashed)
    // On success, the installed outputs (.spv then .json, or the .slang-module).
    std::vector<std::filesystem::path> outputs;
};

// slangc to use: $QUAT_SLANGC, else the build's slangc (QUAT_SLANGC_PATH, set
// by CMake), else the first slangc on PATH. Null when none exists.
[[nodiscard]] std::optional<std::filesystem::path> find_slangc();

class ShaderCompiler {
public:
    // Fails when no slangc is found or the source/output directories are unusable.
    [[nodiscard]] static std::expected<ShaderCompiler, core::Error>
    create(ShaderCompilerConfig config);

    ShaderCompiler() = default;

    [[nodiscard]] const ShaderCompilerConfig& config() const { return config_; }

    // module when `source` lives in the module directory, pass otherwise.
    [[nodiscard]] ShaderSourceKind kind_of(const std::filesystem::path& source) const;

    // Cooked output paths for `source` (.spv + .json, or the .slang-module).
    [[nodiscard]] std::vector<std::filesystem::path>
    outputs_for(const std::filesystem::path& source) const;

    // slangc arguments (argv[1..]) compiling `source` into `out_dir`.
    [[nodiscard]] std::vector<std::string>
    arguments(const std::filesystem::path& source, const std::filesystem::path& out_dir) const;

    // Compiles `source` synchronously. Thread-safe with respect to other
    // sources; concurrent compiles of the same source are not supported.
    // `canceller` (optional) lets another thread kill the running slangc.
    [[nodiscard]] ShaderCompileResult compile(const std::filesystem::path& source,
                                              core::ProcessCanceller* canceller = nullptr) const;

private:
    ShaderCompilerConfig config_;
};

class ShaderDependencyGraph {
public:
    ShaderDependencyGraph() = default;
    // Pass shaders live in `source_dir`, library modules in `module_dir`.
    ShaderDependencyGraph(std::filesystem::path source_dir, std::filesystem::path module_dir);

    // Module names imported by Slang source text: `import a;`, `import a.b;`
    // (-> a/b) and `import "file.slang";`. Comments are ignored.
    [[nodiscard]] static std::vector<std::string> parse_imports(std::string_view source_text);

    // Scans every .slang file in the two directories (non-recursive).
    void scan();

    // Re-reads one source's imports (after an edit), or drops it.
    void update_file(const std::filesystem::path& source);
    void remove_file(const std::filesystem::path& source);
    // Records `source` with the given imports (tests / callers that already
    // hold the text).
    void set_imports(const std::filesystem::path& source, std::vector<std::string> imports);

    // The sources to recompile when `changed` were edited: the changed sources
    // plus every source importing them (transitively). Modules come first in
    // dependency order (imported before importer), then passes, so each source
    // follows everything it imports.
    [[nodiscard]] std::vector<std::filesystem::path>
    rebuild_order(std::span<const std::filesystem::path> changed) const;

    // Sources (in this graph) that `source` imports, directly.
    [[nodiscard]] std::vector<std::filesystem::path>
    imports_of(const std::filesystem::path& source) const;

    [[nodiscard]] bool is_module(const std::filesystem::path& source) const;
    [[nodiscard]] std::vector<std::filesystem::path> sources() const;

private:
    [[nodiscard]] std::optional<std::filesystem::path>
    resolve(const std::filesystem::path& importer, const std::string& import) const;

    std::filesystem::path source_dir_;
    std::filesystem::path module_dir_;
    // Every known source -> the module names it imports (unresolved).
    std::map<std::filesystem::path, std::vector<std::string>> imports_;
};

} // namespace engine::rhi
