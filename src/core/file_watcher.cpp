#include "engine/core/file_watcher.hpp"
#include "engine/core/utf8_path.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
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
//
// mtime + size alone miss a rewrite that lands in the same timestamp tick
// with the same size (NTFS and FAT tick in 10-16 ms / 2 s steps, HFS+ in 1 s).
// As in git's "racy" index entries, a file whose mtime is within
// k_racy_window of the scan is also content-hashed, and while its stored
// stamp is racy an unchanged mtime/size is confirmed by re-hashing. A racy
// file that cannot be read has an unknown hash: nothing is concluded from it.
struct FileStamp {
    fs::file_time_type           mtime{};
    std::uintmax_t               size = 0;
    bool                         racy = false;
    std::optional<std::uint64_t> content_hash; // racy only; nullopt: unreadable
};

constexpr std::chrono::seconds k_racy_window{2};

// FNV-1a over the file's bytes; nullopt when the file cannot be read.
[[nodiscard]] std::optional<std::uint64_t> hash_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::uint64_t h = 1469598103934665603ULL;
    char buf[4096];
    while (file.read(buf, static_cast<std::streamsize>(sizeof(buf))) || file.gcount() > 0) {
        const std::streamsize n = file.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    if (file.bad()) return std::nullopt;
    return h;
}

// Marks `stamp` racy relative to `now` and takes its content hash.
void hash_if_racy(const fs::path& path, FileStamp& stamp, fs::file_time_type now) {
    if (stamp.mtime + k_racy_window < now) return;
    stamp.racy = true;
    stamp.content_hash = hash_file(path);
}

} // namespace

struct FileWatcher::Impl {
    std::vector<fs::path>    directories;
    std::vector<std::string> extensions;
    FileWatchBackend         backend = FileWatchBackend::polling;

    // Changes gathered by the backend, coalesced per path until poll() drains them.
    std::map<fs::path, FileChange::Kind> pending;

    // Last known stamp of every matching file. The polling backend diffs
    // against it each scan; the native backend keeps it current from events
    // and diffs against it when it must rescan (queue overflow, re-watch).
    std::map<fs::path, FileStamp> snapshot;

    // Polling backend.
    std::chrono::milliseconds             poll_interval{250};
    std::chrono::steady_clock::time_point last_scan{};

#if defined(__linux__)
    int inotify_fd = -1;
    std::unordered_map<int, fs::path> watch_dirs;   // watch descriptor -> directory
    std::vector<fs::path>             dropped_dirs; // watch lost; re-added when they reappear
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
        const std::string ext = core::path_to_utf8(path.extension());
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    void record(const fs::path& path, FileChange::Kind kind) {
        if (matches(path)) pending[path] = kind;
    }

    // Current stamps of every matching regular file in `dirs`.
    [[nodiscard]] std::map<fs::path, FileStamp> scan(const std::vector<fs::path>& dirs) const {
        std::map<fs::path, FileStamp> out;
        const fs::file_time_type now = fs::file_time_type::clock::now();
        for (const fs::path& dir : dirs) {
            std::error_code ec;
            fs::directory_iterator it(dir, ec);
            for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
                const fs::path path = dir / it->path().filename();
                if (!matches(path)) continue;
                if (auto stamp = stamp_of(*it)) {
                    hash_if_racy(path, *stamp, now);
                    out.emplace(path, *stamp);
                }
            }
        }
        return out;
    }

    [[nodiscard]] static std::optional<FileStamp> stamp_of(const fs::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec) || ec) return std::nullopt;
        FileStamp stamp;
        stamp.mtime = entry.last_write_time(ec);
        if (ec) return std::nullopt; // removed between listing and stat
        stamp.size = entry.file_size(ec);
        if (ec) return std::nullopt;
        return stamp;
    }

    // Rescans `dirs` and reports the difference from the snapshot: new or
    // changed files as modified, files that are gone as removed.
    void rescan(const std::vector<fs::path>& dirs) {
        const std::map<fs::path, FileStamp> current = scan(dirs);
        for (const auto& [path, stamp] : current) {
            const auto it = snapshot.find(path);
            if (it == snapshot.end()) {
                record(path, FileChange::Kind::modified);
                snapshot[path] = stamp;
                continue;
            }
            const FileStamp& old = it->second;
            bool changed = old.mtime != stamp.mtime || old.size != stamp.size;
            if (!changed && old.racy) {
                // Racy: the same tick may hide a rewrite; compare contents.
                const std::optional<std::uint64_t> now_hash =
                    stamp.racy ? stamp.content_hash : hash_file(path);
                // Unreadable now: unknown — report nothing and keep the old
                // stamp, so the comparison happens once the file is readable.
                if (!now_hash) continue;
                // Unknown before (unreadable when racy): cannot prove it
                // unchanged, so report it.
                changed = !old.content_hash || *now_hash != *old.content_hash;
            }
            if (changed) record(path, FileChange::Kind::modified);
            // Refresh even when unchanged: a racy stamp turns non-racy once its
            // mtime falls out of the window.
            it->second = stamp;
        }
        for (auto it = snapshot.begin(); it != snapshot.end();) {
            const bool in_dirs = std::find(dirs.begin(), dirs.end(), it->first.parent_path()) !=
                                 dirs.end();
            if (in_dirs && !current.contains(it->first)) {
                record(it->first, FileChange::Kind::removed);
                it = snapshot.erase(it);
            } else {
                ++it;
            }
        }
    }

    void poll_scan(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_scan < poll_interval) return;
        last_scan = now;
        rescan(directories);
    }

#if defined(__linux__)
    // IN_CLOSE_WRITE: an in-place save finished. IN_MOVED_TO: a file was
    // renamed into place (editors' atomic save). IN_ATTRIB: `touch`.
    // IN_DELETE_SELF / IN_MOVE_SELF: the directory itself went away.
    static constexpr std::uint32_t k_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB | IN_DELETE |
                                            IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF |
                                            IN_ONLYDIR;

    [[nodiscard]] std::expected<void, Error> init_inotify() {
        inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd < 0) {
            return fail(std::string("inotify_init1 failed: ") + std::strerror(errno));
        }
        for (const fs::path& dir : directories) {
            const int wd = ::inotify_add_watch(inotify_fd, dir.c_str(), k_mask);
            if (wd < 0) {
                return fail("inotify_add_watch(" + dir.string() + ") failed: " +
                            std::strerror(errno));
            }
            watch_dirs.emplace(wd, dir);
        }
        return {};
    }

    // Keeps the snapshot in step with a reported event, so a later rescan
    // (queue overflow, re-watch) diffs against what was already reported.
    void record_event(const fs::path& path, FileChange::Kind kind) {
        if (!matches(path)) return;
        record(path, kind);
        std::error_code ec;
        const std::optional<FileStamp> stamp =
            kind == FileChange::Kind::removed ? std::nullopt
                                              : stamp_of(fs::directory_entry(path, ec));
        if (stamp) {
            snapshot[path] = *stamp;
        } else {
            snapshot.erase(path);
        }
    }

    // A watched directory was deleted, moved away or unmounted: its watch is
    // gone (or, after a move, follows the directory elsewhere — removed here).
    // What it held is reported as removed; poll() re-adds the watch once a
    // directory exists at the path again.
    void drop_watch(int wd) {
        const auto it = watch_dirs.find(wd);
        if (it == watch_dirs.end()) return; // already dropped
        const fs::path dir = it->second;
        watch_dirs.erase(it);
        ::inotify_rm_watch(inotify_fd, wd); // fails harmlessly when already gone
        dropped_dirs.push_back(dir);
        rescan({dir});
    }

    // Re-adds the watch of every dropped directory that exists again, and
    // reports what changed while it was unwatched.
    void retry_dropped() {
        for (auto it = dropped_dirs.begin(); it != dropped_dirs.end();) {
            const int wd = ::inotify_add_watch(inotify_fd, it->c_str(), k_mask);
            if (wd < 0) {
                ++it; // still missing (or not a directory): retry on a later poll
                continue;
            }
            const fs::path dir = *it;
            it = dropped_dirs.erase(it);
            watch_dirs[wd] = dir;
            rescan({dir});
        }
    }

    void drain_inotify() {
        // Large enough for many events; inotify never splits an event across reads.
        alignas(inotify_event) char buffer[16 * 1024];
        for (;;) {
            const ssize_t n = ::read(inotify_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break; // EAGAIN: drained
            }
            for (ssize_t offset = 0; offset < n;) {
                inotify_event event{};
                std::memcpy(&event, buffer + offset, sizeof(inotify_event));
                const char* name = buffer + offset + static_cast<ssize_t>(sizeof(inotify_event));
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event.len);

                if ((event.mask & IN_Q_OVERFLOW) != 0U) {
                    // Events were lost: diff every watched directory against
                    // the snapshot (changes and removals).
                    std::vector<fs::path> watched;
                    for (const auto& [wd, dir] : watch_dirs) watched.push_back(dir);
                    rescan(watched);
                    continue;
                }
                if ((event.mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0U) {
                    drop_watch(event.wd);
                    continue;
                }
                if (event.len == 0 || (event.mask & IN_ISDIR) != 0U) continue;
                const auto dir = watch_dirs.find(event.wd);
                if (dir == watch_dirs.end()) continue;
                const fs::path path = dir->second / fs::path(std::string(name));
                const bool removed = (event.mask & (IN_DELETE | IN_MOVED_FROM)) != 0U;
                record_event(path, removed ? FileChange::Kind::removed : FileChange::Kind::modified);
            }
        }
        if (!dropped_dirs.empty()) retry_dropped();
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
            return fail("FileWatcher: not a directory: " + path_to_utf8(dir));
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
    impl->snapshot = impl->scan(impl->directories); // baseline: existing files are not changes
    impl->last_scan = std::chrono::steady_clock::now();

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
