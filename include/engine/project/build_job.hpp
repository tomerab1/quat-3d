#pragma once

// BuildJob — runs quat-pack (game packaging, ROADMAP P4) as a background
// process for the editor's Build & Export dialog (E7) and the remote
// project.build method, and reports its progress.
//
// quat-pack prints machine-readable progress lines on its output, which the
// job parses as they arrive (BuildProgress):
//   [pack] step <i>/<n> <what>     a step began
//   [pack] package <path>          the archive it wrote
//   [pack] folder <path>           the unpacked package folder
//   [pack] error: <message>        why it failed
// Everything else is log text, kept as a bounded tail.
//
// Thread safety: start(), status() and cancel() may be called from any
// thread (the editor and the remote handlers call them on the main thread);
// the process runs on a worker thread the job owns and joins.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.hpp"
#include "engine/project/package_manifest.hpp"

namespace engine::project {

inline constexpr std::size_t k_build_log_tail_bytes = 64U * 1024U;

// The OS a package is for. Packages are built for the host OS only: each
// needs that OS's player binary and platform tools.
[[nodiscard]] std::string_view host_target(); // "linux-x64", "windows-x64", "macos-arm64"

struct BuildRequest {
    std::filesystem::path quat_pack; // the quat-pack executable
    std::filesystem::path project;   // game project directory (holds game.json)
    std::filesystem::path player;    // the quat-player executable to package
    std::filesystem::path output;    // where the package folder + archive go
    PackageConfig         config = PackageConfig::shipping;
    std::string           build_id;  // optional, appended to the package name
    bool                  smoke_test = false;
};

// quat-pack's argument list for `request` (argv[1..]).
[[nodiscard]] std::vector<std::string> build_arguments(const BuildRequest& request);

enum class BuildState : std::uint8_t { idle, running, succeeded, failed, cancelled };
[[nodiscard]] std::string_view to_string(BuildState state);

struct BuildStatus {
    BuildState    state = BuildState::idle;
    std::uint64_t job = 0;       // increments with every start()
    std::uint32_t step = 0;      // current step (1-based; 0 before the first)
    std::uint32_t steps = 0;     // total steps announced by quat-pack
    std::string   step_name;     // what the current step does
    std::string   log;           // output tail (at most k_build_log_tail_bytes)
    std::string   package;       // archive path, once written
    std::string   folder;        // unpacked package folder, once written
    std::string   error;         // failure reason (failed / cancelled)
    double        elapsed_s = 0.0;

    // 0..1: finished steps over announced steps (1 once succeeded).
    [[nodiscard]] float progress() const;
};

// Incremental parser for quat-pack's output (chunks may split lines).
class BuildProgress {
public:
    // Appends `chunk` to the log tail and applies every complete line.
    void feed(std::string_view chunk, BuildStatus& status);
    // Applies a trailing line without a newline (at process exit).
    void flush(BuildStatus& status);

private:
    void apply_line(std::string_view line, BuildStatus& status);
    std::string pending_;
};

class BuildJob {
public:
    BuildJob();
    ~BuildJob(); // cancels a running build and joins the worker
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;
    BuildJob(BuildJob&&) = delete;
    BuildJob& operator=(BuildJob&&) = delete;

    // Starts quat-pack for `request`. Fails when a build is already running
    // or the request is incomplete (the error says which part).
    [[nodiscard]] std::expected<std::uint64_t, core::Error> start(BuildRequest request);
    // A copy of the state; `with_log` false leaves the log out (cheap
    // per-frame polling).
    [[nodiscard]] BuildStatus status(bool with_log = true) const;
    [[nodiscard]] bool running() const;
    // Kills a running build (its status becomes cancelled). No-op otherwise.
    void cancel();

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

} // namespace engine::project
