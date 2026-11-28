#pragma once

// MaterialShaderRegistry — authored material shaders by name (ROADMAP A6):
// what the shader.* remote methods (and through them an AI assistant) list,
// read, create, edit and delete.
//
// A material shader is a Slang module assets/shaders/materials/<name>.slang
// implementing IMaterial (see material_shader_names.hpp for the file layout).
// The registry owns no compiler: it writes the source and asks the running
// ShaderHotReload to compile it — the same service, worker thread, staging
// and pipeline swap as an edit saved from a text editor — then reports the
// outcome with slangc's diagnostics parsed into located entries.
//
// create / update are transactional: the file on disk only keeps the new text
// when it compiles. A failed create removes the file again; a failed update
// restores the previous text (which then recompiles, so the status returns to
// that version). Either way the cooked programs, and therefore the running
// pipelines, were never touched — the previous version keeps rendering.
// Operations run one at a time in request order; the reply comes from poll()
// once the compile has finished.
//
// Main thread only (like the hot reload service it drives). Vulkan-free.

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/rhi/shader_diagnostics.hpp"

namespace engine::rhi {

class ShaderHotReload;

class MaterialShaderRegistry {
public:
    struct Config {
        std::filesystem::path material_dir;  // assets/shaders/materials
        std::filesystem::path module_dir;    // assets/shaders/lib (stems are reserved names)
        std::filesystem::path template_path; // starter module (shader.template)
        std::filesystem::path builtin_path;  // lib/material.slang: the interface reference
        std::filesystem::path cooked_dir;    // cooked/shaders
        // Paths in results and diagnostics are made relative to this.
        std::filesystem::path display_root;
        std::size_t max_source_bytes = 256 * 1024;
        // How long an operation may wait for its compile once the service has
        // picked it up; raised to stay above the compiler's own timeout.
        std::chrono::milliseconds compile_timeout{90'000};
    };

    enum class ErrorKind : std::uint8_t {
        invalid,     // bad name / source (the message says which)
        not_found,   // no such material shader
        exists,      // create of an existing name
        unavailable, // no compiler service (shader hot reload off)
        failed,      // file system error, timeout
    };
    struct Error {
        ErrorKind   kind = ErrorKind::failed;
        std::string message;
    };

    struct Info {
        std::string   name;
        bool          builtin = false;
        std::string   path;  // display path of the source
        // builtin | cooked (as built offline) | compiling | ok | error |
        // not_compiled (no cooked program and no compiler service)
        std::string   state;
        std::uint32_t reloads = 0;    // successful recompiles this session
        std::string   message;        // compiler output of the last compile
        std::vector<ShaderDiagnostic> diagnostics; // parsed from `message`
    };

    struct Outcome {
        std::string name;
        std::string path;
        bool        ok = false;
        // Failed update: the previous source was restored and keeps running.
        bool        kept_previous = false;
        std::vector<ShaderDiagnostic> diagnostics;
        std::string log; // the compiler output, paths shortened
    };
    using Done = std::function<void(std::expected<Outcome, Error>)>;

    // `reload` may be null: listing and reading work, compiling answers
    // `unavailable`.
    MaterialShaderRegistry(Config config, ShaderHotReload* reload);

    [[nodiscard]] const Config& config() const { return config_; }
    [[nodiscard]] bool can_compile() const { return reload_ != nullptr; }
    // How long an operation waits for its compile (see Config::compile_timeout).
    [[nodiscard]] std::chrono::milliseconds timeout() const;

    // The built-in material first, then every material shader source by name.
    [[nodiscard]] std::vector<Info> list() const;
    [[nodiscard]] std::expected<Info, Error> info(std::string_view name) const;
    // Source text of a material shader (the built-in: lib/material.slang).
    [[nodiscard]] std::expected<std::string, Error> source(std::string_view name) const;
    // The starter module text.
    [[nodiscard]] std::expected<std::string, Error> template_source() const;

    // Name rules (material_shaders::valid_name, not the built-in name, not a
    // library module); the error explains the rule that failed.
    [[nodiscard]] std::expected<void, Error> check_name(std::string_view name) const;

    // Queue a create / update; `done` runs from a later poll() (or right away
    // for errors detectable up front).
    void create(std::string name, std::string source, Done done);
    void update(std::string name, std::string source, Done done);

    // Deletes the source and its cooked programs. Refused while an operation
    // on it is queued or compiling.
    [[nodiscard]] std::expected<void, Error> remove(std::string_view name);

    // Once per frame on the main thread, after ShaderHotReload::update().
    void poll();

    // Queued + running operations.
    [[nodiscard]] std::size_t pending() const { return ops_.size(); }

    // Absolute paths of a material's source and cooked programs.
    [[nodiscard]] std::filesystem::path source_path(std::string_view name) const;
    [[nodiscard]] std::vector<std::filesystem::path> cooked_paths(std::string_view name) const;

    // `text` with absolute project paths shortened to display paths.
    [[nodiscard]] std::string shorten_paths(std::string text) const;

private:
    struct Op {
        enum class Kind : std::uint8_t { create, update };
        enum class Phase : std::uint8_t { queued, compiling, restoring };
        Kind        kind = Kind::create;
        Phase       phase = Phase::queued;
        std::string name;
        std::string text;
        std::string previous; // update: the text to restore on failure
        Done        done;
        std::uint32_t attempts_before = 0;
        std::chrono::steady_clock::time_point started{};
        bool        dispatched = false; // the service has started compiling it
    };

    void start(Op& op);
    void finish_compile(Op& op);
    // A compile that failed or timed out: undo the write, reply, and (update)
    // wait for the restored source to recompile.
    void fail_compile(Op& op, Outcome out);
    void time_out(Op& op);
    [[nodiscard]] std::vector<std::string> library_modules() const;
    [[nodiscard]] std::uint32_t attempts_of(const std::filesystem::path& source) const;
    [[nodiscard]] std::vector<ShaderDiagnostic> located(std::string_view output) const;
    [[nodiscard]] std::string display(const std::filesystem::path& path) const;
    // A file name slangc printed (UTF-8, native separators, possibly another
    // spelling of the same directory) as a display path when it lies under
    // the display root; otherwise unchanged.
    [[nodiscard]] std::string display_printed(const std::string& printed) const;

    Config           config_;
    // config_.display_root resolved (fs::weakly_canonical): reported paths are
    // compared in resolved form, since tools may print another spelling of
    // the same directory (a Windows 8.3 short name such as RUNNER~1 in the
    // temp path, a symlink).
    std::filesystem::path canonical_root_;
    ShaderHotReload* reload_ = nullptr; // non-owning
    std::deque<Op>   ops_;              // front() is the running one
    bool             polling_ = false;
};

[[nodiscard]] std::string_view to_string(MaterialShaderRegistry::ErrorKind kind);

} // namespace engine::rhi
