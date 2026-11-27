// run_process (R2): captures stdout + stderr and the exit status of a child
// process without going through a shell; a missing program is an error.

#include <string>
#include <vector>

#include <gtest/gtest.h>

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

#endif

TEST(ChildProcess, MissingProgramFails) {
    EXPECT_FALSE(engine::core::run_process("/nonexistent/quat-no-such-program", {}).has_value());
}

} // namespace
