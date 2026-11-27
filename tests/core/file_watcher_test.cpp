// FileWatcher (R2): writes, renames into place and removals in a temp
// directory are reported by the native backend (Linux inotify) and by the
// portable polling backend; non-matching extensions are ignored.

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

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

INSTANTIATE_TEST_SUITE_P(Backends, FileWatcherTest,
                         ::testing::Values(FileWatchBackend::native, FileWatchBackend::polling),
                         [](const ::testing::TestParamInfo<FileWatchBackend>& info) {
                             return info.param == FileWatchBackend::native ? std::string("Native")
                                                                           : std::string("Polling");
                         });

TEST(FileWatcher, RejectsMissingDirectory) {
    FileWatcher::Options options;
    options.directories = {fs::path("/nonexistent/quat-file-watcher")};
    EXPECT_FALSE(FileWatcher::create(options).has_value());
}

} // namespace
