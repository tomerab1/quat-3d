#pragma once

// FileWatcher — reports files created, modified, renamed into place or removed
// in a set of directories (ROADMAP R2: shader hot reload).
//
// Backends:
//   * native  — Linux inotify (IN_CLOSE_WRITE / IN_MOVED_TO / IN_ATTRIB for
//               writes and renames into place, IN_DELETE / IN_MOVED_FROM for
//               removals). A queue overflow triggers a full rescan.
//   * polling — a periodic std::filesystem scan comparing modification time
//               and size. Works everywhere; used on platforms without a native
//               backend (Windows, macOS) and when the native backend fails.
//
// Directories are watched non-recursively (list subdirectories explicitly).
// poll() never blocks: call it from the main loop. Not thread-safe; one owner.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::core {

enum class FileWatchBackend : std::uint8_t {
    automatic, // native when the platform has one, else polling
    native,    // fail creation when there is no native backend
    polling,
};

struct FileChange {
    enum class Kind : std::uint8_t { modified, removed };
    std::filesystem::path path; // watched directory / file name
    Kind                  kind = Kind::modified;
};

class FileWatcher {
public:
    struct Options {
        std::vector<std::filesystem::path> directories;
        // File extensions to report, with the dot (".slang"); empty = all.
        std::vector<std::string> extensions;
        FileWatchBackend backend = FileWatchBackend::automatic;
        // Minimum time between directory scans of the polling backend.
        std::chrono::milliseconds poll_interval{250};
    };

    [[nodiscard]] static std::expected<FileWatcher, Error> create(Options options);

    FileWatcher();
    ~FileWatcher();
    FileWatcher(FileWatcher&& other) noexcept;
    FileWatcher& operator=(FileWatcher&& other) noexcept;
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Files changed since the previous call: one entry per path (the latest
    // kind wins), sorted by path. Non-blocking.
    [[nodiscard]] std::vector<FileChange> poll();

    // The backend actually in use (native or polling).
    [[nodiscard]] FileWatchBackend backend() const;

    // True when this platform has a native backend.
    [[nodiscard]] static bool native_backend_available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::core
