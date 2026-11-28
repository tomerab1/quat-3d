// FileWatcher (R2): writes, renames into place and removals in a temp
// directory are reported by the native backend (Linux inotify) and by the
// portable polling backend; non-matching extensions are ignored; a watched
// directory that is deleted and recreated is watched again; an inotify queue
// overflow still reports removals.

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "engine/core/file_watcher.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using engine::core::FileChange;
using engine::core::FileWatchBackend;
using engine::core::FileWatcher;

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Polls until `pred` holds for the accumulated changes or ~2 s pass.
template <typename Pred>
std::vector<FileChange> poll_until(FileWatcher& watcher, Pred pred) {
    std::vector<FileChange> all;
    for (int i = 0; i < 200; ++i) {
        for (FileChange& c : watcher.poll()) all.push_back(std::move(c));
        if (pred(all)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return all;
}

bool has(const std::vector<FileChange>& changes, const fs::path& path, FileChange::Kind kind) {
    for (const FileChange& c : changes) {
        if (c.path == path && c.kind == kind) return true;
    }
    return false;
}

class FileWatcherTest : public ::testing::TestWithParam<FileWatchBackend> {
protected:
    void SetUp() override {
        if (GetParam() == FileWatchBackend::native && !FileWatcher::native_backend_available()) {
            GTEST_SKIP() << "no native file-watch backend on this platform";
        }
    }

    [[nodiscard]] std::expected<FileWatcher, engine::core::Error> make_watcher() {
        FileWatcher::Options options;
        options.directories = {dir_.path()};
        options.extensions = {".slang"};
        options.backend = GetParam();
        options.poll_interval = std::chrono::milliseconds(0);
        return FileWatcher::create(options);
    }

    quat::test::TempDir dir_{"file_watcher"};
};

TEST_P(FileWatcherTest, ReportsInPlaceWrites) {
    const fs::path file = dir_.path() / "a.slang";
    write_file(file, "first");
    auto watcher = make_watcher();
    ASSERT_OK(watcher);
    EXPECT_EQ(watcher->backend(), GetParam());
    EXPECT_TRUE(watcher->poll().empty()) << "existing files are not changes";

    write_file(file, "second version");
    const auto changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, file, FileChange::Kind::modified);
    });
    EXPECT_TRUE(has(changes, file, FileChange::Kind::modified));
}

// A same-size rewrite inside one timestamp tick (NTFS/FAT/HFS+ granularity)
// leaves mtime and size unchanged; the polling backend must still see it.
// Simulated portably by restoring the old mtime after the rewrite.
TEST(FileWatcherPolling, ReportsSameSizeRewriteWithinOneTimestampTick) {
    quat::test::TempDir dir{"file_watcher_racy"};
    const fs::path file = dir.path() / "r.slang";
    write_file(file, "value 1");
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(file, ec);
    ASSERT_FALSE(ec) << ec.message();

    FileWatcher::Options options;
    options.directories = {dir.path()};
    options.extensions = {".slang"};
    options.backend = FileWatchBackend::polling;
    options.poll_interval = std::chrono::milliseconds(0);
    auto watcher = FileWatcher::create(options);
    ASSERT_OK(watcher);
    EXPECT_TRUE(watcher->poll().empty());

    write_file(file, "value 5"); // same size
    fs::last_write_time(file, stamp, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(has(watcher->poll(), file, FileChange::Kind::modified));
    EXPECT_TRUE(watcher->poll().empty()) << "reported once";
}

// A racy file that cannot be read is "unknown": nothing is reported and the
// old stamp is kept, so the same-tick rewrite is still caught once the file
// is readable again. POSIX permissions only; root reads mode-000 files.
TEST(FileWatcherPolling, UnreadableRacyFileIsUnknownUntilReadable) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX permission bits";
#else
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";
    quat::test::TempDir dir{"file_watcher_unreadable"};
    const fs::path file = dir.path() / "u.slang";
    write_file(file, "value 1");
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(file, ec);
    ASSERT_FALSE(ec) << ec.message();

    FileWatcher::Options options;
    options.directories = {dir.path()};
    options.extensions = {".slang"};
    options.backend = FileWatchBackend::polling;
    options.poll_interval = std::chrono::milliseconds(0);
    auto watcher = FileWatcher::create(options);
    ASSERT_OK(watcher);

    write_file(file, "value 5"); // same size, same tick (mtime restored)
    fs::last_write_time(file, stamp, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::permissions(file, fs::perms::none, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(watcher->poll().empty()) << "unreadable: unknown, not reported";

    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(has(watcher->poll(), file, FileChange::Kind::modified));
#endif
}

TEST_P(FileWatcherTest, ReportsRenameIntoPlace) {
    const fs::path file = dir_.path() / "b.slang";
    write_file(file, "old");
    auto watcher = make_watcher();
    ASSERT_OK(watcher);

    // Editors' atomic save: write a temp file, rename it over the target. The
    // temp file's extension is filtered out.
    const fs::path tmp = dir_.path() / "b.slang.tmp";
    write_file(tmp, "new contents, longer");
    std::error_code ec;
    fs::rename(tmp, file, ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, file, FileChange::Kind::modified);
    });
    EXPECT_TRUE(has(changes, file, FileChange::Kind::modified));
    for (const FileChange& c : changes) {
        EXPECT_EQ(c.path.extension(), ".slang") << "unfiltered change: " << c.path;
    }
}

TEST_P(FileWatcherTest, ReportsNewAndRemovedFiles) {
    auto watcher = make_watcher();
    ASSERT_OK(watcher);
    const fs::path created = dir_.path() / "c.slang";
    write_file(created, "hello");
    auto changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, created, FileChange::Kind::modified);
    });
    EXPECT_TRUE(has(changes, created, FileChange::Kind::modified));

    std::error_code ec;
    fs::remove(created, ec);
    changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, created, FileChange::Kind::removed);
    });
    EXPECT_TRUE(has(changes, created, FileChange::Kind::removed));
}

TEST_P(FileWatcherTest, CoalescesRepeatedWrites) {
    auto watcher = make_watcher();
    ASSERT_OK(watcher);
    const fs::path file = dir_.path() / "d.slang";
    write_file(file, "1");
    write_file(file, "22");
    write_file(file, "333");
    const auto changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, file, FileChange::Kind::modified);
    });
    std::size_t count = 0;
    for (const FileChange& c : changes) count += c.path == file ? 1 : 0;
    EXPECT_GE(count, 1U);
    EXPECT_TRUE(watcher->poll().empty()) << "changes are drained by poll()";
}

TEST_P(FileWatcherTest, WatchesARecreatedDirectoryAgain) {
    const fs::path sub = dir_.path() / "sub";
    fs::create_directories(sub);
    const fs::path old_file = sub / "old.slang";
    write_file(old_file, "old");
    FileWatcher::Options options;
    options.directories = {sub};
    options.extensions = {".slang"};
    options.backend = GetParam();
    options.poll_interval = std::chrono::milliseconds(0);
    auto watcher = FileWatcher::create(options);
    ASSERT_OK(watcher);

    std::error_code ec;
    fs::remove_all(sub, ec);
    ASSERT_FALSE(ec) << ec.message();
    auto changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, old_file, FileChange::Kind::removed);
    });
    EXPECT_TRUE(has(changes, old_file, FileChange::Kind::removed));
    EXPECT_TRUE(watcher->poll().empty());

    // Recreated: files written before the watch is back are found by the
    // rescan, and later edits arrive as ordinary events.
    fs::create_directories(sub);
    const fs::path file = sub / "new.slang";
    write_file(file, "first");
    changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, file, FileChange::Kind::modified);
    });
    EXPECT_TRUE(has(changes, file, FileChange::Kind::modified));
    EXPECT_FALSE(has(changes, old_file, FileChange::Kind::modified));

    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // distinct mtime
    write_file(file, "second, longer");
    changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, file, FileChange::Kind::modified);
    });
    EXPECT_TRUE(has(changes, file, FileChange::Kind::modified)) << "edit after re-watch lost";
}

INSTANTIATE_TEST_SUITE_P(Backends, FileWatcherTest,
                         ::testing::Values(FileWatchBackend::native, FileWatchBackend::polling),
                         [](const ::testing::TestParamInfo<FileWatchBackend>& info) {
                             return info.param == FileWatchBackend::native ? std::string("Native")
                                                                           : std::string("Polling");
                         });

#if defined(__linux__)
// Overflows the real inotify queue (more unread events than
// fs.inotify.max_queued_events), so the deletion and the edit made after it
// are lost as events; the rescan must still report both.
TEST(FileWatcher, QueueOverflowReportsRemovalsAndChanges) {
    long max_events = 0;
    {
        std::ifstream in("/proc/sys/fs/inotify/max_queued_events");
        in >> max_events;
    }
    if (max_events <= 0 || max_events > (1L << 20)) {
        GTEST_SKIP() << "cannot overflow an inotify queue of " << max_events << " events";
    }
    quat::test::TempDir dir("file_watcher_overflow");
    const fs::path gone = dir.path() / "gone.slang";
    const fs::path kept = dir.path() / "kept.slang";
    const fs::path flood[] = {dir.path() / "flood_a.tmp", dir.path() / "flood_b.tmp"};
    write_file(gone, "gone");
    write_file(kept, "kept");
    for (const fs::path& f : flood) write_file(f, "");

    FileWatcher::Options options;
    options.directories = {dir.path()};
    options.extensions = {".slang"};
    options.backend = FileWatchBackend::native;
    auto watcher = FileWatcher::create(options);
    ASSERT_OK(watcher);

    // chmod queues IN_ATTRIB (watched) on non-matching files; alternating
    // files so inotify cannot merge consecutive identical events.
    for (long i = 0; i < max_events + 64; ++i) {
        std::error_code ec;
        fs::permissions(flood[i & 1], fs::perms::owner_read | fs::perms::owner_write, ec);
    }
    std::error_code ec;
    fs::remove(gone, ec);
    ASSERT_FALSE(ec);
    write_file(kept, "kept, but edited");

    const auto changes = poll_until(*watcher, [&](const auto& all) {
        return has(all, gone, FileChange::Kind::removed) &&
               has(all, kept, FileChange::Kind::modified);
    });
    EXPECT_TRUE(has(changes, gone, FileChange::Kind::removed));
    EXPECT_TRUE(has(changes, kept, FileChange::Kind::modified));
}
#endif

TEST(FileWatcher, RejectsMissingDirectory) {
    FileWatcher::Options options;
    options.directories = {fs::path("/nonexistent/quat-file-watcher")};
    EXPECT_FALSE(FileWatcher::create(options).has_value());
}

} // namespace
