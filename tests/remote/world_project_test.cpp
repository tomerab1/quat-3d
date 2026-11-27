// project.info / project.build / project.build_status (ROADMAP P4) against
// fake services: parameter validation, output confinement, status JSON.

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/project/build_job.hpp"
#include "remote/world_fixture.hpp"
#include "support/temp_project.hpp"

namespace {

namespace fs = std::filesystem;
namespace code = engine::remote::rpc_code;
namespace project = engine::project;
using quat::test::Json;

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

class WorldProject : public quat::test::WorldCommands {
protected:
    quat::test::TempProject                      root{"quat_world_project"};
    bool                                         open = true;
    std::vector<engine::remote::ProjectBuildParams> builds;
    project::BuildStatus                         status;

    void SetUp() override {
        services.game_project = [this]() -> std::optional<engine::remote::GameProjectInfo> {
            if (!open) return std::nullopt;
            project::GameProject p;
            p.name = "Coin Collector";
            p.id = "dev.quat3d.coin-collector";
            p.version = "0.1.0";
            p.start_scene = "assets/scenes/main.json";
            p.content = {"assets/scripts"};
            return engine::remote::GameProjectInfo{root.path(), p};
        };
        services.project_build = [this](const engine::remote::ProjectBuildParams& p)
            -> std::expected<std::uint64_t, engine::core::Error> {
            if (p.target == "windows-x64") {
                return std::unexpected(engine::core::Error{"cannot package for windows-x64"});
            }
            builds.push_back(p);
            return builds.size();
        };
        services.project_build_status = [this] { return status; };
        register_commands();
    }
};

TEST_F(WorldProject, Info) {
    const Json info = ok("project.info");
    EXPECT_TRUE(info["open"].get<bool>());
    EXPECT_EQ(info["game"]["name"], "Coin Collector");
    EXPECT_EQ(info["game"]["format"], 1);
    EXPECT_EQ(info["executable"], "coin-collector");
    EXPECT_EQ(info["host_target"], std::string(project::host_target()));
    EXPECT_EQ(info["build"]["state"], "idle");
    err("project.info", {{"verbose", true}}, code::invalid_params);

    open = false;
    const Json closed = ok("project.info");
    EXPECT_FALSE(closed["open"].get<bool>());
    EXPECT_TRUE(contains(closed["reason"].get<std::string>(), "QUAT_GAME"));
    EXPECT_TRUE(contains(err("project.build", Json::object(), code::failed), "no game project"));
}

TEST_F(WorldProject, BuildValidatesAndConfinesItsParameters) {
    const Json r = ok("project.build", {{"config", "development"}, {"smoke_test", true}});
    EXPECT_EQ(r["job"], 1);
    EXPECT_EQ(r["state"], "running");
    EXPECT_EQ(r["output"], "dist");
    EXPECT_EQ(r["config"], "development");
    ASSERT_EQ(builds.size(), 1U);
    EXPECT_EQ(builds[0].output, root.path() / "dist");
    EXPECT_EQ(builds[0].config, project::PackageConfig::development);
    EXPECT_TRUE(builds[0].smoke_test);

    ok("project.build", {{"output", "out/packages"}, {"target", std::string(project::host_target())}});
    EXPECT_EQ(builds.back().output, root.path() / "out" / "packages");

    EXPECT_TRUE(contains(err("project.build", {{"output", "../escape"}}, code::invalid_params),
                         "output:"));
    // Packages may not land in, or swallow, what the game ships.
    EXPECT_EQ(err("project.build", {{"output", "assets/scripts/dist"}}, code::invalid_params),
              "output: 'assets/scripts/dist' is inside the content root 'assets/scripts'");
    EXPECT_EQ(err("project.build", {{"output", "assets"}}, code::invalid_params),
              "output: 'assets' contains the content root 'assets/scripts'");
    EXPECT_TRUE(contains(err("project.build", {{"output", "assets/scenes"}}, code::invalid_params),
                         "is the start scene's folder"));
    EXPECT_TRUE(contains(err("project.build", {{"output", "/abs"}}, code::invalid_params), "output:"));
    EXPECT_TRUE(contains(err("project.build", {{"target", "amiga"}}, code::invalid_params),
                         "target:"));
    EXPECT_TRUE(contains(err("project.build", {{"config", "debug"}}, code::invalid_params),
                         "config:"));
    EXPECT_TRUE(contains(err("project.build", {{"targets", "x"}}, code::invalid_params),
                         "unknown field"));
    // A refusal from the service (another OS, a build already running).
    EXPECT_TRUE(contains(err("project.build", {{"target", "windows-x64"}}, code::failed),
                         "cannot package"));
}

TEST_F(WorldProject, BuildStatus) {
    status.state = project::BuildState::running;
    status.job = 3;
    status.step = 2;
    status.steps = 4;
    status.step_name = "collect game files";
    status.log = "one\ntwo\nthree\n";
    status.folder = (root.path() / "dist" / "coin-collector-0.1.0-linux-x64").string();
    status.package = (root.path() / "dist" / "coin-collector-0.1.0-linux-x64.tar.gz").string();
    const Json s = ok("project.build_status", {{"log_lines", 2}});
    EXPECT_EQ(s["job"], 3);
    EXPECT_EQ(s["state"], "running");
    EXPECT_FLOAT_EQ(s["progress"].get<float>(), 0.25F);
    EXPECT_EQ(s["step_name"], "collect game files");
    EXPECT_EQ(s["log"], (Json{"two", "three"}));
    EXPECT_EQ(s["package"], "dist/coin-collector-0.1.0-linux-x64.tar.gz"); // project-relative
    EXPECT_EQ(s["folder"], "dist/coin-collector-0.1.0-linux-x64");
    EXPECT_EQ(ok("project.build_status")["log"].size(), 3U);
    EXPECT_TRUE(ok("project.build_status", {{"log_lines", 0}})["log"].empty());
    err("project.build_status", {{"log_lines", -1}}, code::invalid_params);
}

class WorldNoProjectServices : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

TEST_F(WorldNoProjectServices, Unavailable) {
    err("project.info", Json::object(), code::unavailable);
    err("project.build", Json::object(), code::unavailable);
    err("project.build_status", Json::object(), code::unavailable);
}

} // namespace
