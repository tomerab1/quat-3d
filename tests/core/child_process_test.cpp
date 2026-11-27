// run_process (R2): captures stdout + stderr and the exit status of a child
// process without going through a shell; a missing program is an error; a
// child that outlives its timeout, or is cancelled from another thread, is
// killed (with anything it started) and reported as an error promptly — also
// when it closed its output first (#11) — and is always reaped.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "engine/core/child_process.hpp"
#include "support/expect_ok.hpp"

namespace {

#if !defined(_WIN32)

TEST(ChildProcess, CapturesOutputAndExitCode) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value()) << "no sh on PATH";
    const std::vector<std::string> args = {"-c", "echo to-stdout; echo to-stderr >&2; exit 3"};
    auto r = engine::core::run_process(*sh, args);
    ASSERT_OK(r);
    EXPECT_EQ(r->exit_code, 3);
    EXPECT_FALSE(r->signaled);
    EXPECT_FALSE(r->succeeded());
    EXPECT_NE(r->output.find("to-stdout"), std::string::npos) << r->output;
    EXPECT_NE(r->output.find("to-stderr"), std::string::npos) << r->output;
}

TEST(ChildProcess, PassesArgumentsVerbatim) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    // No shell parses these: spaces and quotes reach the program intact.
    const std::vector<std::string> args = {"-c", "printf '%s|' \"$@\"", "sh", "a b", "\"q\"", "$x"};
    auto r = engine::core::run_process(*sh, args);
    ASSERT_OK(r);
    EXPECT_TRUE(r->succeeded());
    EXPECT_EQ(r->output, "a b|\"q\"|$x|");
}

TEST(ChildProcess, ReportsSignals) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    const std::vector<std::string> args = {"-c", "kill -SEGV $$"};
    auto r = engine::core::run_process(*sh, args);
    ASSERT_OK(r);
    EXPECT_TRUE(r->signaled);
    EXPECT_FALSE(r->succeeded());
}

using Clock = std::chrono::steady_clock;

// `echo` first so the shell forks `sleep` instead of exec'ing it: the
// grandchild holds the pipe open, so only killing the process group returns.
const std::vector<std::string> k_hang = {"-c", "echo started; sleep 30; echo never"};

TEST(ChildProcess, TimesOutAndKillsAHungChild) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    engine::core::ProcessOptions options;
    options.timeout = std::chrono::milliseconds(200);
    const Clock::time_point start = Clock::now();
    auto r = engine::core::run_process(*sh, k_hang, options);
    const auto elapsed = Clock::now() - start;
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("timed out"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("started"), std::string::npos) << r.error().message;
    EXPECT_EQ(r.error().message.find("never"), std::string::npos) << r.error().message;
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(ChildProcess, TimeoutLeavesAFastChildAlone) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    engine::core::ProcessOptions options;
    options.timeout = std::chrono::seconds(30);
    const std::vector<std::string> args = {"-c", "echo hi; exit 4"};
    auto r = engine::core::run_process(*sh, args, options);
    ASSERT_OK(r);
    EXPECT_EQ(r->exit_code, 4);
    EXPECT_EQ(r->output, "hi\n");
}

TEST(ChildProcess, CancelFromAnotherThreadKillsTheChild) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    engine::core::ProcessCanceller canceller;
    engine::core::ProcessOptions options;
    options.canceller = &canceller;
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        canceller.cancel();
    });
    const Clock::time_point start = Clock::now();
    auto r = engine::core::run_process(*sh, k_hang, options);
    const auto elapsed = Clock::now() - start;
    killer.join();
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("cancelled"), std::string::npos) << r.error().message;
    EXPECT_LT(elapsed, std::chrono::seconds(5));

    // A cancelled canceller stops later runs at once.
    EXPECT_TRUE(canceller.cancelled());
    const Clock::time_point again = Clock::now();
    EXPECT_ERR(engine::core::run_process(*sh, k_hang, options));
    EXPECT_LT(Clock::now() - again, std::chrono::seconds(5));
}

// Prints its pid, closes stdout/stderr and keeps running: the read loop sees
// EOF at once, so only the wait for the exit itself can time out.
const std::vector<std::string> k_silent_hang = {"-c", "echo $$; exec >&- 2>&-; sleep 30"};

// The pid a k_silent_hang child printed, from the error's "output so far".
pid_t printed_pid(const std::string& message) {
    constexpr std::string_view marker = "output so far:\n";
    const std::size_t at = message.find(marker);
    if (at == std::string::npos) return -1;
    pid_t pid = -1;
    std::from_chars(message.data() + at + marker.size(), message.data() + message.size(), pid);
    return pid;
}

#if defined(__linux__)
// Live (not zombie) processes in process group `pgid`, from /proc. A killed
// grandchild is reparented to init and stays a zombie until init reaps it,
// which can take a second in a container, so kill(-pgid, 0) is no test.
int live_processes_in_group(pid_t pgid) {
    int live = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc", ec), end; !ec && it != end;
         it.increment(ec)) {
        std::ifstream in(it->path() / "stat");
        std::string stat;
        if (!std::getline(in, stat)) continue;
        // "pid (comm) state ppid pgrp ...": comm may hold spaces, so parse
        // after its closing parenthesis.
        const std::size_t paren = stat.rfind(')');
        if (paren == std::string::npos) continue;
        std::istringstream fields(stat.substr(paren + 1));
        char state = 0;
        long ppid = 0;
        long pgrp = 0;
        if (fields >> state >> ppid >> pgrp && pgrp == pgid && state != 'Z') ++live;
    }
    return live;
}
#endif

// The child was reaped (waitpid knows no such child of ours) and nothing it
// started is still running.
void expect_reaped_with_its_group(pid_t pid) {
    ASSERT_GT(pid, 0);
    errno = 0;
    EXPECT_EQ(::waitpid(pid, nullptr, WNOHANG), -1) << "child was not reaped";
    EXPECT_EQ(errno, ECHILD);
#if defined(__linux__)
    // SIGKILL delivery to the rest of the group is asynchronous.
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds(2);
    while (live_processes_in_group(pid) > 0 && Clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(live_processes_in_group(pid), 0) << "the child's process group is still running";
#endif
}

TEST(ChildProcess, TimesOutAChildThatClosedItsOutput) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    engine::core::ProcessOptions options;
    options.timeout = std::chrono::milliseconds(200);
    const Clock::time_point start = Clock::now();
    auto r = engine::core::run_process(*sh, k_silent_hang, options);
    const auto elapsed = Clock::now() - start;
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("timed out"), std::string::npos) << r.error().message;
    EXPECT_GE(elapsed, std::chrono::milliseconds(200));
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    expect_reaped_with_its_group(printed_pid(r.error().message));
}

TEST(ChildProcess, CancelsAChildThatClosedItsOutput) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    engine::core::ProcessCanceller canceller;
    engine::core::ProcessOptions options;
    options.canceller = &canceller;
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        canceller.cancel();
    });
    const Clock::time_point start = Clock::now();
    auto r = engine::core::run_process(*sh, k_silent_hang, options);
    const auto elapsed = Clock::now() - start;
    killer.join();
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("cancelled"), std::string::npos) << r.error().message;
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    expect_reaped_with_its_group(printed_pid(r.error().message));
}

// A child that closes its output a little before it exits is waited for: not
// taken as finished at EOF, and not killed.
TEST(ChildProcess, WaitsForAChildThatExitsAfterClosingItsOutput) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    engine::core::ProcessOptions options;
    options.timeout = std::chrono::seconds(30);
    const std::vector<std::string> args = {"-c", "echo bye; exec >&- 2>&-; sleep 0.2; exit 5"};
    auto r = engine::core::run_process(*sh, args, options);
    ASSERT_OK(r);
    EXPECT_EQ(r->exit_code, 5);
    EXPECT_EQ(r->output, "bye\n");
}

// Spawns racing on two threads: if a slow child inherited a fast call's
// pipe, that call would wait for the slow child before seeing EOF.
TEST(ChildProcess, ConcurrentCallsDoNotInheritEachOthersPipes) {
    const auto sh = engine::core::find_on_path("sh");
    ASSERT_TRUE(sh.has_value());
    std::atomic<bool> slow_done{false};
    std::thread slow([&] {
        const std::vector<std::string> args = {"-c", "sleep 0.5"};
        for (int i = 0; i < 4; ++i) EXPECT_OK(engine::core::run_process(*sh, args));
        slow_done = true;
    });
    const std::vector<std::string> quick = {"-c", "echo quick"};
    Clock::duration worst{};
    int runs = 0;
    while (!slow_done) {
        const Clock::time_point start = Clock::now();
        auto r = engine::core::run_process(*sh, quick);
        worst = std::max(worst, Clock::now() - start);
        ++runs;
        EXPECT_OK(r);
        if (r) EXPECT_EQ(r->output, "quick\n");
    }
    slow.join();
    EXPECT_GT(runs, 0);
    EXPECT_LT(worst, std::chrono::milliseconds(400))
        << "a quick call waited on another call's child";
}

#endif

TEST(ChildProcess, MissingProgramFails) {
    EXPECT_FALSE(engine::core::run_process("/nonexistent/quat-no-such-program", {}).has_value());
}

} // namespace
