#pragma once

// ShaderHotReload — ROADMAP R2. Watches the Slang sources, recompiles edited
// shaders (and the shaders importing an edited library module) with slangc on
// a worker thread, and swaps the affected pipelines on the main thread at the
// start of the next frame. Material shaders (assets/shaders/materials, ROADMAP
// A6) are watched and compiled the same way, into one program per pass
// variant. A failed compile keeps the old pipelines and the
// cooked files untouched; the diagnostics are exposed per shader (statuses())
// for the editor and remote commands.
//
// Enabled by QUAT_SHADER_HOT_RELOAD=1 (=0 disables); defaults on in editor
// builds (ENGINE_EDITOR) and off otherwise, so shipping builds never watch
// files.
//
// Frame-loop integration (main thread):
//   auto reload = ShaderHotReload::create_from_environment(cache, src_dir, cooked_dir);
//   ... each frame, after waiting for the frame's in-flight fence and before
//   recording:  if (reload) reload->update(frame_number);

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <vector>

#include "engine/core/error.hpp"
#include "engine/core/file_watcher.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_status.hpp"

namespace engine::rhi {

class PipelineCache;

class ShaderHotReload {
public:
    struct Config {
        std::filesystem::path source_dir;  // assets/shaders
        std::filesystem::path output_dir;  // cooked/shaders (the dir passes load from)
        std::filesystem::path module_dir;  // empty: source_dir / "lib"
        std::filesystem::path material_dir; // empty: source_dir / "materials"
        std::filesystem::path slangc;      // empty: find_slangc()
        core::FileWatchBackend    watch_backend = core::FileWatchBackend::automatic;
        std::chrono::milliseconds poll_interval{250}; // polling watcher only
        // Changes are compiled once a file has been quiet this long (editors
        // often write a file in several steps).
        std::chrono::milliseconds debounce{80};
        // A slangc run exceeding this is killed and reported as an error.
        std::chrono::milliseconds compile_timeout{60'000};
        std::uint32_t frames_in_flight = 2;
        // Compile on a worker thread (false: compile inside update(); tests).
        bool async = true;
    };

    // QUAT_SHADER_HOT_RELOAD=1/0 when set, else the build default (on with
    // ENGINE_EDITOR).
    [[nodiscard]] static bool enabled_by_environment();

    // Starts watching. Fails when slangc or the directories are missing.
    [[nodiscard]] static std::expected<std::unique_ptr<ShaderHotReload>, core::Error>
    create(PipelineCache& cache, Config config);

    // create() when enabled_by_environment(), else null. A creation failure is
    // logged to stderr and also yields null (hot reload is a dev convenience).
    [[nodiscard]] static std::unique_ptr<ShaderHotReload>
    create_from_environment(PipelineCache& cache, Config config);
    // Same, with the default Config for these directories.
    [[nodiscard]] static std::unique_ptr<ShaderHotReload>
    create_from_environment(PipelineCache& cache, const std::filesystem::path& source_dir,
                            const std::filesystem::path& output_dir);

    ~ShaderHotReload();
    ShaderHotReload(const ShaderHotReload&) = delete;
    ShaderHotReload& operator=(const ShaderHotReload&) = delete;

    // Once per frame on the main thread, after waiting for the in-flight fence
    // of the frame about to be recorded (`frame_number` counts submitted
    // frames): destroys pipelines retired >= frames_in_flight frames ago,
    // polls the watcher, queues settled changes for compilation and applies
    // finished compiles (rebuilt pipelines are used from this frame on).
    void update(std::uint64_t frame_number);

    // Queues a recompile of `source` (and its importers) as if it had changed.
    // For tools/remote commands. A source the service does not know yet is
    // picked up when it exists in one of the watched directories (a file just
    // written); anything else is ignored. Returns whether it was queued.
    bool request_recompile(const std::filesystem::path& source);

    // One entry per known shader source, sorted by name.
    [[nodiscard]] const std::vector<ShaderStatus>& statuses() const;
    // The status of `source` (an absolute path as in ShaderStatus::source),
    // or null when the service does not track it.
    [[nodiscard]] const ShaderStatus* status_of(const std::filesystem::path& source) const;
    // Directories and slangc the service compiles with (normalised).
    [[nodiscard]] const ShaderCompilerConfig& compiler_config() const;
    [[nodiscard]] std::size_t error_count() const;
    // No change pending, queued or compiling.
    [[nodiscard]] bool idle() const;
    [[nodiscard]] core::FileWatchBackend watch_backend() const;

private:
    struct Impl;
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

public:
    ShaderHotReload(PrivateTag, std::unique_ptr<Impl> impl); // use create()

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::rhi
