// BuildJob (ROADMAP P4/E7): quat-pack's progress protocol, its argument
// list, and a background run against a stand-in quat-pack (POSIX shell
// script).

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "engine/project/build_job.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

namespace fs = std::filesystem;
namespace project = engine::project;
using project::BuildState;

TEST(BuildProgress, ParsesStepsAcrossChunkBoundaries) {
    project::BuildStatus st;
    project::BuildProgress p;
    p.feed("hello\n[pack] st", st);
    EXPECT_EQ(st.step, 0U);
    p.feed("ep 2/5 collect game files\r\n[pack] step x/5 bad\n", st);
    EXPECT_EQ(st.step, 2U);
    EXPECT_EQ(st.steps, 5U);
    EXPECT_EQ(st.step_name, "collect game files");
    st.state = BuildState::running;
    EXPECT_FLOAT_EQ(st.progress(), 0.2F); // one of five steps done

    p.feed("[pack] folder /out/game-1.0.0-linux-x64\n[pack] package /out/game.tar.gz", st);
    EXPECT_EQ(st.folder, "/out/game-1.0.0-linux-x64");
    EXPECT_TRUE(st.package.empty()); // no newline yet
    p.flush(st);
    EXPECT_EQ(st.package, "/out/game.tar.gz");

    p.feed("[pack] error: first\n[pack] error: second\n", st);
    EXPECT_EQ(st.error, "first");
    EXPECT_NE(st.log.find("collect game files"), std::string::npos);
    st.state = BuildState::succeeded;
    EXPECT_FLOAT_EQ(st.progress(), 1.0F);
}

TEST(BuildProgress, LogTailIsBounded) {
    project::BuildStatus st;
    project::BuildProgress p;
    const std::string line(1000, 'x');
    for (int i = 0; i < 200; ++i) p.feed(line + "\n", st);
    EXPECT_LE(st.log.size(), project::k_build_log_tail_bytes);
    EXPECT_EQ(st.log.front(), 'x'); // cut at a line start
}

TEST(BuildJob, Arguments) {
    project::BuildRequest r;
    r.project = "games/demo";
    r.player = "bin/quat-player";
    r.output = "dist";
    r.config = project::PackageConfig::development;
    r.build_id = "dev.1";
    r.smoke_test = true;
    const auto args = project::build_arguments(r);
    const std::vector<std::string> want{"games/demo", "--player", "bin/quat-player", "--out",
                                       "dist", "--config", "development", "--build-id", "dev.1",
                                       "--smoke-test"};
    EXPECT_EQ(args, want);
}

TEST(BuildJob, RefusesIncompleteRequests) {
    project::BuildJob job;
    project::BuildRequest r;
    EXPECT_ERR(job.start(r));
    r.quat_pack = "/no/such/quat-pack";
    r.project = "p";
    r.player = "/no/such/player";
    r.output = "o";
    auto e = job.start(r);
    ASSERT_ERR(e);
    EXPECT_NE(e.error().message.find("quat-pack not found"), std::string::npos);
    EXPECT_EQ(job.status().state, BuildState::idle);
}

#if !defined(_WIN32)
project::BuildStatus wait_done(const project::BuildJob& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (job.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return job.status();
}

TEST(BuildJob, RunsInTheBackgroundAndReportsProgress) {
    quat::test::TempProject dir("quat_build_job");
    dir.write("quat-pack", "#!/bin/sh\n"
                           "echo \"[pack] step 1/2 validate game project\"\n"
                           "echo \"args: $*\"\n"
                           "echo \"[pack] step 2/2 lay out\"\n"
                           "echo \"[pack] package /tmp/x.tar.gz\"\n");
    dir.write("player", "");
    fs::permissions(dir.path() / "quat-pack", fs::perms::owner_all);
    project::BuildJob job;
    project::BuildRequest r;
    r.quat_pack = dir.path() / "quat-pack";
    r.player = dir.path() / "player";
    r.project = "proj";
    r.output = "out";
    auto started = job.start(r);
    ASSERT_OK(started);
    EXPECT_EQ(*started, 1U);
    const project::BuildStatus st = wait_done(job);
    EXPECT_EQ(st.state, BuildState::succeeded) << st.error << "\n" << st.log;
    EXPECT_EQ(st.step, 2U);
    EXPECT_EQ(st.package, "/tmp/x.tar.gz");
    EXPECT_NE(st.log.find("args: proj --player"), std::string::npos) << st.log;
    EXPECT_TRUE(job.status(/*with_log=*/false).log.empty());

    // A failing quat-pack: its error line wins over the exit code.
    dir.write("quat-pack", "#!/bin/sh\necho \"[pack] error: no game.json\"\nexit 1\n");
    ASSERT_OK(job.start(r));
    const project::BuildStatus failed = wait_done(job);
    EXPECT_EQ(failed.job, 2U);
    EXPECT_EQ(failed.state, BuildState::failed);
    EXPECT_EQ(failed.error, "no game.json");
}

TEST(BuildJob, Cancel) {
    quat::test::TempProject dir("quat_build_job");
    dir.write("quat-pack", "#!/bin/sh\necho \"[pack] step 1/1 wait\"\nsleep 30\n");
    dir.write("player", "");
    fs::permissions(dir.path() / "quat-pack", fs::perms::owner_all);
    project::BuildJob job;
    project::BuildRequest r;
    r.quat_pack = dir.path() / "quat-pack";
    r.player = dir.path() / "player";
    r.project = "p";
    r.output = "o";
    ASSERT_OK(job.start(r));
    EXPECT_ERR(job.start(r)) << "a second build while one runs";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    job.cancel();
    const project::BuildStatus st = wait_done(job);
    EXPECT_EQ(st.state, BuildState::cancelled);
}
#endif

} // namespace
