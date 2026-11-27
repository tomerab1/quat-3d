// StderrCapture: stderr writes land in the ring as parsed lines AND still
// reach the original stderr; partial lines, bursts larger than the pipe,
// writes from other threads, log() bypassing the capture, restore on stop.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "engine/core/log.hpp"
#include "engine/core/stderr_capture.hpp"
#include "support/expect_ok.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using engine::core::LogLevel;
using engine::core::LogQuery;
using engine::core::LogRing;
using engine::core::StderrCapture;

// Points the process's real stderr at a scratch file for the test (so the
// forwarded copy can be checked and test output stays quiet); restores it.
class StderrCaptureTest : public ::testing::Test {
protected:
    fs::path path_;
    int      saved_ = -1;

    void SetUp() override {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("quat_stderr_" + std::to_string(rd()) + ".log");
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        ASSERT_GE(saved_, 0);
        const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_GE(::dup2(fd, STDERR_FILENO), 0);
        ::close(fd);
    }
    void TearDown() override {
        std::fflush(stderr);
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
        std::error_code ec;
        fs::remove(path_, ec);
    }
    [[nodiscard]] std::string forwarded() const {
        std::ifstream in(path_, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    static std::vector<engine::core::LogLine> lines(const LogRing& ring, std::size_t limit = 100) {
        LogQuery q;
        q.since_seq = 0;
        q.limit = limit;
        return ring.query(q).lines;
    }
};

TEST_F(StderrCaptureTest, RoundTripsLinesIntoTheRingAndTheRealStderr) {
    LogRing ring(64);
    auto capture = StderrCapture::start(ring);
    ASSERT_OK(capture);
    EXPECT_TRUE(capture->active());

    std::fprintf(stderr, "[warn] primitive resolve: missing mesh\n");
    std::fprintf(stderr, "[nav] baked %d polygons\n", 42);
    const char raw[] = "partial ";
    ASSERT_EQ(::write(STDERR_FILENO, raw, sizeof(raw) - 1), static_cast<ssize_t>(sizeof(raw) - 1));
    capture->flush();

    auto got = lines(ring);
    ASSERT_EQ(got.size(), 2U) << "the partial line must wait for its newline";
    EXPECT_EQ(got[0].level, LogLevel::warn);
    EXPECT_EQ(got[0].category, "stderr");
    EXPECT_EQ(got[0].message, "primitive resolve: missing mesh");
    EXPECT_EQ(got[1].category, "nav");
    EXPECT_EQ(got[1].message, "baked 42 polygons");

    std::fprintf(stderr, "line\n");
    capture->flush();
    got = lines(ring);
    ASSERT_EQ(got.size(), 3U);
    EXPECT_EQ(got[2].message, "partial line");

    capture->stop();
    EXPECT_FALSE(capture->active());
    // After stop, stderr is the file again and nothing more is captured.
    std::fprintf(stderr, "after stop\n");
    std::fflush(stderr);
    EXPECT_EQ(lines(ring).size(), 3U);
    EXPECT_EQ(forwarded(),
              "[warn] primitive resolve: missing mesh\n[nav] baked 42 polygons\npartial line\n"
              "after stop\n");
}

TEST_F(StderrCaptureTest, OnlyOneCaptureAtATime) {
    LogRing ring(8);
    auto first = StderrCapture::start(ring);
    ASSERT_OK(first);
    auto second = StderrCapture::start(ring);
    EXPECT_FALSE(second.has_value());
    first->stop();
    auto third = StderrCapture::start(ring);
    EXPECT_OK(third);
}

TEST_F(StderrCaptureTest, BurstLargerThanThePipeDoesNotBlock) {
    // 4 MiB from the main thread with no flush() in between: the drain
    // thread must keep the pipe empty, or this write would block forever.
    LogRing ring(1U << 16);
    auto capture = StderrCapture::start(ring);
    ASSERT_OK(capture);
    const std::string line(1023, 'x');
    constexpr int k_lines = 4096;
    for (int i = 0; i < k_lines; ++i) {
        ASSERT_EQ(::write(STDERR_FILENO, (line + "\n").data(), line.size() + 1),
                  static_cast<ssize_t>(line.size() + 1));
    }
    capture->stop();
    EXPECT_EQ(lines(ring, k_lines + 10).size(), static_cast<std::size_t>(k_lines));
    EXPECT_EQ(forwarded().size(), static_cast<std::size_t>(k_lines) * 1024U);
}

TEST_F(StderrCaptureTest, WritesFromOtherThreadsAreCaptured) {
    LogRing ring(4096);
    auto capture = StderrCapture::start(ring);
    ASSERT_OK(capture);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 100; ++i) std::fprintf(stderr, "[worker] %d %d\n", t, i);
        });
    }
    for (auto& th : threads) th.join();
    capture->flush();
    const auto got = lines(ring, 1000);
    EXPECT_EQ(got.size(), 400U);
    for (const auto& l : got) EXPECT_EQ(l.category, "worker");
}

TEST_F(StderrCaptureTest, LogHelperIsRecordedOnce) {
    // core::log() writes the ring directly and echoes to the real stderr,
    // bypassing the pipe: the global ring must not see it twice.
    auto capture = StderrCapture::start(engine::core::log_ring());
    ASSERT_OK(capture);
    const std::uint64_t before = engine::core::log_ring().last_seq();
    engine::core::log(LogLevel::warn, "test", "value {}", 7);
    capture->flush();
    LogQuery q;
    q.since_seq = before;
    const auto r = engine::core::log_ring().query(q);
    ASSERT_EQ(r.lines.size(), 1U);
    EXPECT_EQ(r.lines[0].category, "test");
    EXPECT_EQ(r.lines[0].message, "value 7");
    capture->stop();
    EXPECT_EQ(forwarded(), "[test][warn] value 7\n");
}

} // namespace

#endif
