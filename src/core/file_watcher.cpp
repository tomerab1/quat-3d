#include "engine/core/file_watcher.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace engine::core {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// Modification stamp the polling backend compares between scans.
struct FileStamp {
    fs::file_time_type mtime{};
    std::uintmax_t     size = 0;

    [[nodiscard]] bool operator==(const FileStamp&) const = default;
};

} // namespace

struct FileWatcher::Impl {
    std::vector<fs::path>    directories;
    std::vector<std::string> extensions;
    FileWatchBackend         backend = FileWatchBackend::polling;

    // Changes gathered by the backend, coalesced per path until poll() drains them.
    std::map<fs::path, FileChange::Kind> pending;

    // Polling backend.
    std::chrono::milliseconds             poll_interval{250};
    std::chrono::steady_clock::time_point last_scan{};
    std::map<fs::path, FileStamp>         snapshot;

#if defined(__linux__)
    int inotify_fd = -1;
    std::unordered_map<int, fs::path> watch_dirs; // watch descriptor -> directory
#endif

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
#if defined(__linux__)
        if (inotify_fd >= 0) ::close(inotify_fd);
#endif
    }

    [[nodiscard]] bool matches(const fs::path& path) const {
        if (extensions.empty()) return true;
        const std::string ext = path.extension().string();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    void record(const fs::path& path, FileChange::Kind kind) {
        if (matches(path)) pending[path] = kind;
    }

    // Current stamps of every matching regular file in the watched directories.
    [[nodiscard]] std::map<fs::path, FileStamp> scan() const {
        std::map<fs::path, FileStamp> out;
        for (const fs::path& dir : directories) {
            std::error_code ec;
            fs::directory_iterator it(dir, ec);
            for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
                const fs::directory_entry& entry = *it;
                std::error_code entry_ec;
                if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
                const fs::path path = dir / entry.path().filename();
                if (!matches(path)) continue;
                FileStamp stamp;
                stamp.mtime = entry.last_write_time(entry_ec);
                if (entry_ec) continue; // removed between listing and stat
                stamp.size = entry.file_size(entry_ec);
                if (entry_ec) continue;
                out.emplace(path, stamp);
            }
        }
        return out;
    }

    void poll_scan(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_scan < poll_interval) return;
        last_scan = now;
        std::map<fs::path, FileStamp> current = scan();
        for (const auto& [path, stamp] : current) {
            const auto it = snapshot.find(path);
            if (it == snapshot.end() || !(it->second == stamp)) {
                record(path, FileChange::Kind::modified);
            }
        }
        for (const auto& [path, stamp] : snapshot) {
            if (!current.contains(path)) record(path, FileChange::Kind::removed);
        }
        snapshot = std::move(current);
    }

#if defined(__linux__)
    [[nodiscard]] std::expected<void, Error> init_inotify() {
        inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd < 0) {
            return fail(std::string("inotify_init1 failed: ") + std::strerror(errno));
        }
        // IN_CLOSE_WRITE: an in-place save finished. IN_MOVED_TO: a file was
        // renamed into place (editors' atomic save). IN_ATTRIB: `touch`.
        constexpr std::uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB | IN_DELETE |
                                       IN_MOVED_FROM | IN_ONLYDIR;
        for (const fs::path& dir : directories) {
            const int wd = ::inotify_add_watch(inotify_fd, dir.c_str(), mask);
            if (wd < 0) {
                return fail("inotify_add_watch(" + dir.string() + ") failed: " +
                            std::strerror(errno));
            }
            watch_dirs.emplace(wd, dir);
        }
        return {};
    }

    void drain_inotify() {
        // Large enough for many events; inotify never splits an event across reads.
        alignas(inotify_event) char buffer[16 * 1024];
        for (;;) {
            const ssize_t n = ::read(inotify_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return; // EAGAIN: drained
            }
            for (ssize_t offset = 0; offset < n;) {
                inotify_event event{};
                std::memcpy(&event, buffer + offset, sizeof(inotify_event));
                const char* name = buffer + offset + static_cast<ssize_t>(sizeof(inotify_event));
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event.len);

                if ((event.mask & IN_Q_OVERFLOW) != 0U) {
                    // Events were lost: report every file as modified.
                    for (const auto& [path, stamp] : scan()) {
                        record(path, FileChange::Kind::modified);
                    }
                    continue;
                }
                if ((event.mask & IN_IGNORED) != 0U) {
                    watch_dirs.erase(event.wd);
                    continue;
                }
                if (event.len == 0 || (event.mask & IN_ISDIR) != 0U) continue;
                const auto dir = watch_dirs.find(event.wd);
                if (dir == watch_dirs.end()) continue;
                const fs::path path = dir->second / fs::path(std::string(name));
                const bool removed = (event.mask & (IN_DELETE | IN_MOVED_FROM)) != 0U;
                record(path, removed ? FileChange::Kind::removed : FileChange::Kind::modified);
            }
        }
    }
#endif
};

bool FileWatcher::native_backend_available() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

std::expected<FileWatcher, Error> FileWatcher::create(Options options) {
    if (options.directories.empty()) {
        return fail("FileWatcher: no directories to watch");
    }
    auto impl = std::make_unique<Impl>();
    for (fs::path& dir : options.directories) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return fail("FileWatcher: not a directory: " + dir.string());
        }
        impl->directories.push_back(dir.lexically_normal());
    }
    impl->extensions = std::move(options.extensions);
    impl->poll_interval = options.poll_interval;

    bool use_native = options.backend != FileWatchBackend::polling && native_backend_available();
    if (options.backend == FileWatchBackend::native && !use_native) {
        return fail("FileWatcher: no native file-watch backend on this platform");
    }
#if defined(__linux__)
    if (use_native) {
        if (auto r = impl->init_inotify(); !r) {
            if (options.backend == FileWatchBackend::native) return std::unexpected(r.error());
            // automatic: fall back to polling (e.g. inotify watch limit reached).
            if (impl->inotify_fd >= 0) ::close(impl->inotify_fd);
            impl->inotify_fd = -1;
            impl->watch_dirs.clear();
            use_native = false;
        }
    }
#endif
    impl->backend = use_native ? FileWatchBackend::native : FileWatchBackend::polling;
    if (!use_native) {
        impl->snapshot = impl->scan(); // baseline: existing files are not changes
        impl->last_scan = std::chrono::steady_clock::now();
    }

    FileWatcher out;
    out.impl_ = std::move(impl);
    return out;
}

FileWatcher::FileWatcher() = default;
FileWatcher::~FileWatcher() = default;
FileWatcher::FileWatcher(FileWatcher&& other) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&& other) noexcept = default;

std::vector<FileChange> FileWatcher::poll() {
    if (!impl_) return {};
#if defined(__linux__)
    if (impl_->backend == FileWatchBackend::native) {
        impl_->drain_inotify();
    } else {
        impl_->poll_scan(/*force=*/false);
    }
#else
    impl_->poll_scan(/*force=*/false);
#endif
    std::vector<FileChange> out;
    out.reserve(impl_->pending.size());
    for (const auto& [path, kind] : impl_->pending) {
        out.push_back(FileChange{path, kind});
    }
    impl_->pending.clear();
    return out;
}

FileWatchBackend FileWatcher::backend() const {
    return impl_ ? impl_->backend : FileWatchBackend::polling;
}

} // namespace engine::core
