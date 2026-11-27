// environment.*, world.ground_height, camera.*, asset.*, sim.*, scene.save/
// load and nav.bake against fake WorldServices; path confinement.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "remote/world_fixture.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace fs = std::filesystem;
using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;
using engine::remote::CameraPose;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A scratch project root, removed afterwards.
class TempRoot {
public:
    TempRoot() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("quat_world_test_" + std::to_string(rd()));
        fs::create_directories(path_ / "assets" / "models");
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }
    void write(const std::string& relative, const std::string& text) const {
        fs::create_directories((path_ / relative).parent_path());
        std::ofstream(path_ / relative) << text;
    }

private:
    fs::path path_;
};

// No services at all (a bare engine / tests): app capabilities are unavailable.
class WorldNoServices : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

TEST_F(WorldNoServices, AppCapabilitiesAreUnavailable) {
    for (const char* method : {"camera.get", "camera.frame", "asset.list_models", "sim.play",
                               "sim.stop", "nav.bake"}) {
        err(method, Json::object(), code::unavailable);
    }
    err("camera.set", {{"fov_deg", 50}}, code::unavailable);
    err("world.ground_height", {{"points", {{0, 0}}}}, code::unavailable);
    err("scene.save", {{"path", "a.json"}}, code::unavailable);
    err("scene.load", {{"path", "a.json"}}, code::unavailable);
    err("asset.spawn_model", {{"path", "assets/a.glb"}}, code::unavailable);
    err("environment.set", {{"clouds", {{"coverage", 0.5}}}}, code::unavailable);
    // Deferred methods cannot run through call() (or inside a batch).
    err("render.screenshot", Json::object(), code::unavailable);
    // The sun works on any scene; clouds report null without the service.
    const Json env = ok("environment.get");
    EXPECT_TRUE(env["sun"].is_null());
    EXPECT_TRUE(env["clouds"].is_null());
    // Bad parameters are still reported as such first where they are checked.
    err("world.ground_height", {{"points", {{0}}}}, code::invalid_params);
}

TEST_F(WorldNoServices, EnvironmentSunRoundTrip) {
    const Json set = ok("environment.set", {{"sun", {{"azimuth_deg", 30},
                                                     {"elevation_deg", 45},
                                                     {"intensity", 3.5},
                                                     {"color", {1.0, 0.9, 0.8}}}}});
    EXPECT_NEAR(set["sun"]["azimuth_deg"].get<float>(), 30.0F, 1e-3F);
    EXPECT_NEAR(set["sun"]["elevation_deg"].get<float>(), 45.0F, 1e-3F);
    EXPECT_FLOAT_EQ(set["sun"]["intensity"].get<float>(), 3.5F);

    // The scene gained a sun whose travel direction follows the convention.
    const auto suns = reg().view<s::DirectionalLight>();
    ASSERT_EQ(suns.size(), 1U);
    const glm::vec3 d = suns.get<s::DirectionalLight>(*suns.begin()).direction;
    EXPECT_NEAR(glm::degrees(std::atan2(d.z, d.x)), 30.0F, 1e-3F);
    EXPECT_NEAR(glm::degrees(std::asin(-d.y)), 45.0F, 1e-3F);

    // Partial update keeps the other angle; a hex colour is sRGB.
    const Json moved = ok("environment.set", {{"sun", {{"elevation_deg", 10}, {"color", "#ffffff"}}}});
    EXPECT_NEAR(moved["sun"]["azimuth_deg"].get<float>(), 30.0F, 1e-3F);
    EXPECT_NEAR(moved["sun"]["elevation_deg"].get<float>(), 10.0F, 1e-3F);
    EXPECT_FLOAT_EQ(moved["sun"]["color"][1].get<float>(), 1.0F);
    EXPECT_EQ(reg().view<s::DirectionalLight>().size(), 1U); // reused, not duplicated
    EXPECT_EQ(ok("environment.get")["sun"], moved["sun"]);

    std::string msg = err("environment.set", {{"sun", {{"elevation_deg", 120}}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "sun.elevation_deg")) << msg;
    msg = err("environment.set", {{"sun", {{"azimuth", 10}}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "sun.azimuth: unknown field")) << msg;
    err("environment.set", {{"sun", {{"color", "red"}}}}, code::invalid_params);
}

class WorldWithServices : public quat::test::WorldCommands {
protected:
    TempRoot               root;
    CameraPose             camera{{0, 2, 10}, -90.0F, 0.0F, 60.0F};
    engine::renderer::CloudSettings clouds;
    bool                   playing = false;
    std::vector<fs::path>  saved;
    std::vector<fs::path>  loaded;
    std::vector<fs::path>  spawned;
    int                    spawn_roots = 1;

    void SetUp() override {
        services.project_root = [this] { return root.path(); };
        services.camera_get = [this] { return camera; };
        services.camera_set = [this](const CameraPose& p) { camera = p; };
        services.clouds_get = [this] { return clouds; };
        services.clouds_set = [this](const engine::renderer::CloudSettings& c) { clouds = c; };
        services.ground_height = [](float x, float z) -> std::optional<float> {
            if (x < 0.0F) return std::nullopt;
            return x + 2.0F * z;
        };
        services.set_playing = [this](bool play) -> std::expected<bool, engine::core::Error> {
            playing = play;
            return playing;
        };
        services.playing = [this] { return playing; };
        services.save_scene = [this](const fs::path& p) -> std::expected<void, engine::core::Error> {
            saved.push_back(p);
            return {};
        };
        services.load_scene = [this](const fs::path& p) -> std::expected<void, engine::core::Error> {
            loaded.push_back(p);
            return {};
        };
        services.nav_bake = []() -> std::expected<std::size_t, engine::core::Error> { return 42; };
        services.spawn_model = [this](const fs::path& p)
            -> std::expected<std::vector<entt::entity>, engine::core::Error> {
            spawned.push_back(p);
            std::vector<entt::entity> roots;
            for (int i = 0; i < spawn_roots; ++i) {
                roots.push_back(scene.create_entity("node" + std::to_string(i)));
            }
            return roots;
        };
        register_commands();
    }
};

TEST_F(WorldWithServices, CloudsAndGroundHeight) {
    const Json env = ok("environment.set", {{"clouds", {{"coverage", 0.8}, {"enabled", false}}}});
    EXPECT_FLOAT_EQ(clouds.coverage, 0.8F);
    EXPECT_FALSE(clouds.enabled);
    EXPECT_FLOAT_EQ(env["clouds"]["coverage"].get<float>(), 0.8F);
    EXPECT_FLOAT_EQ(env["clouds"]["density"].get<float>(), clouds.density); // kept
    // Validation precedes every change: a bad sun leaves the clouds alone.
    err("environment.set", {{"clouds", {{"coverage", 0.1}}}, {"sun", {{"intensity", -1}}}},
        code::invalid_params);
    EXPECT_FLOAT_EQ(clouds.coverage, 0.8F);
    err("environment.set", {{"clouds", {{"coverage", 2}}}}, code::invalid_params);

    const Json h = ok("world.ground_height", {{"points", {{1, 2}, {-5, 0}}}});
    EXPECT_FLOAT_EQ(h["heights"][0].get<float>(), 5.0F);
    EXPECT_TRUE(h["heights"][1].is_null());
    Json many = Json::array();
    for (int i = 0; i < 1025; ++i) many.push_back({0, 0});
    err("world.ground_height", {{"points", many}}, code::invalid_params);
}

TEST_F(WorldWithServices, CameraSetLookAtAndFrame) {
    Json got = ok("camera.get");
    EXPECT_FLOAT_EQ(got["fov_deg"].get<float>(), 60.0F);

    got = ok("camera.set", {{"position", {0, 0, 0}}, {"look_at", {10, 0, 0}}, {"fov_deg", 45}});
    EXPECT_NEAR(camera.yaw_deg, 0.0F, 1e-4F); // +X
    EXPECT_NEAR(camera.pitch_deg, 0.0F, 1e-4F);
    EXPECT_FLOAT_EQ(camera.fov_deg, 45.0F);
    ok("camera.set", {{"look_at", {0, 10, 10}}});
    EXPECT_NEAR(camera.yaw_deg, 90.0F, 1e-3F); // +Z
    EXPECT_NEAR(camera.pitch_deg, 45.0F, 1e-3F);
    err("camera.set", {{"look_at", {1, 1, 1}}, {"yaw_deg", 3}}, code::invalid_params);
    err("camera.set", {{"pitch_deg", 95}}, code::invalid_params);
    err("camera.set", {{"look_at", {0, 0, 0}}}, code::invalid_params); // at the camera

    // Frame a 2 m cube at (20, 1, 0): the camera ends up looking at it.
    ok("scene.create", {{"name", "target"},
                        {"components", {{"transform", {{"position", {20, 1, 0}}, {"scale", 2}}},
                                        {"primitive", {{"shape", "cube"}}}}}});
    got = ok("camera.frame", {{"entity", "target"}});
    const float yaw = glm::radians(camera.yaw_deg);
    const float pitch = glm::radians(camera.pitch_deg);
    const glm::vec3 fwd(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw));
    const glm::vec3 to_target = glm::normalize(glm::vec3(20, 1, 0) - camera.position);
    EXPECT_GT(glm::dot(fwd, to_target), 0.999F);
    EXPECT_LE(camera.pitch_deg, -20.0F + 1e-3F);
    const float dist = glm::length(glm::vec3(20, 1, 0) - camera.position);
    EXPECT_GT(dist, std::sqrt(3.0F)); // outside the cube's bounding sphere
    EXPECT_LT(dist, 10.0F);
    ok("camera.frame"); // whole scene
    err("camera.frame", {{"entity", "missing"}}, code::not_found);
}

TEST_F(WorldWithServices, PathsAreConfinedToTheProjectRoot) {
    for (const char* bad : {"../outside.json", "/etc/passwd.json", "a/../../x.json", "", "."}) {
        const std::string msg = err("scene.save", {{"path", bad}}, code::invalid_params);
        EXPECT_FALSE(msg.empty()) << bad;
    }
    EXPECT_TRUE(saved.empty());
    err("scene.save", {{"path", "scenes/level.txt"}}, code::invalid_params); // not .json

    EXPECT_EQ(ok("scene.save", {{"path", "scenes/./level.json"}})["path"], "scenes/level.json");
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved[0], root.path() / "scenes/level.json");

    // Existing non-scene JSON is never overwritten; existing scenes are.
    root.write("vcpkg.json", R"({"name": "x"})");
    root.write("scenes/old.json", R"({"version": 2, "entities": []})");
    EXPECT_TRUE(contains(err("scene.save", {{"path", "vcpkg.json"}}, code::invalid_params),
                         "not a scene file"));
    ok("scene.save", {{"path", "scenes/old.json"}});

    err("scene.load", {{"path", "scenes/missing.json"}}, code::not_found);
    EXPECT_EQ(ok("scene.load", {{"path", "scenes/old.json"}})["path"], "scenes/old.json");
    ASSERT_EQ(loaded.size(), 1U);
    err("scene.load", {{"path", "../../etc/hosts"}}, code::invalid_params);

    // Symlinks may not lead out of the project either.
    std::error_code ec;
    fs::create_directory_symlink(fs::temp_directory_path(), root.path() / "escape", ec);
    if (!ec) err("scene.save", {{"path", "escape/x.json"}}, code::invalid_params);
}

TEST(ConfineToRoot, RejectsEscapes) {
    const fs::path root = fs::temp_directory_path();
    EXPECT_OK(engine::remote::confine_to_root(root, "a/b.json"));
    EXPECT_OK(engine::remote::confine_to_root(root, "a/../b.json"));
    EXPECT_ERR(engine::remote::confine_to_root(root, ".."));
    EXPECT_ERR(engine::remote::confine_to_root(root, "../x"));
    EXPECT_ERR(engine::remote::confine_to_root(root, "a/../../x"));
    EXPECT_ERR(engine::remote::confine_to_root(root, "/abs"));
    EXPECT_ERR(engine::remote::confine_to_root(root, ""));
    EXPECT_ERR(engine::remote::confine_to_root(root, std::string("a\0b", 3)));
    const auto ok = engine::remote::confine_to_root(root, "x/./y.json");
    ASSERT_OK(ok);
    EXPECT_EQ(*ok, root / "x/y.json");
}

TEST_F(WorldWithServices, ModelsSpawnAndGroup) {
    root.write("assets/models/tree.glb", "glb");
    root.write("assets/models/sub/House.GLTF", "{}");
    root.write("assets/models/readme.txt", "x");
    const Json models = ok("asset.list_models");
    EXPECT_EQ(models["models"],
              Json::array({"assets/models/sub/House.GLTF", "assets/models/tree.glb"}));

    // One root: name and components apply to it directly.
    Json r = ok("asset.spawn_model",
                {{"path", "assets/models/tree.glb"},
                 {"name", "oak"},
                 {"components", {{"transform", {{"position", {3, 0, 4}}}}}}});
    ASSERT_EQ(r["roots"].size(), 1U);
    EXPECT_EQ(spawned.back(), root.path() / "assets/models/tree.glb");
    const entt::entity oak = entity(r["roots"][0]);
    EXPECT_EQ(reg().get<s::Name>(oak).value, "oak");
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(oak).world[3].z, 4.0F);

    // Several roots + a name: grouped under one entity.
    spawn_roots = 3;
    r = ok("asset.spawn_model", {{"path", "assets/models/sub/House.GLTF"}, {"name", "house"}});
    ASSERT_EQ(r["roots"].size(), 1U);
    const Json group = ok("scene.get", {{"entity", r["roots"][0]}});
    EXPECT_EQ(group["name"], "house");
    EXPECT_EQ(group["children"].size(), 3U);
    // ... and without any of them, the roots are returned as they are.
    r = ok("asset.spawn_model", {{"path", "assets/models/tree.glb"}});
    EXPECT_EQ(r["roots"].size(), 3U);

    const std::size_t calls = spawned.size();
    err("asset.spawn_model", {{"path", "assets/models/readme.txt"}}, code::invalid_params);
    err("asset.spawn_model", {{"path", "assets/models/none.glb"}}, code::not_found);
    err("asset.spawn_model", {{"path", "../x.glb"}}, code::invalid_params);
    err("asset.spawn_model",
        {{"path", "assets/models/tree.glb"}, {"components", {{"primitive", {{"shape", "blob"}}}}}},
        code::invalid_params);
    EXPECT_EQ(spawned.size(), calls); // nothing loaded for rejected requests
}

TEST_F(WorldWithServices, PlayStopAndNavBake) {
    EXPECT_EQ(ok("sim.play")["playing"], true);
    EXPECT_TRUE(playing);
    EXPECT_EQ(ok("sim.stop")["playing"], false);
    EXPECT_EQ(ok("nav.bake")["polygons"], 42);
    err("sim.play", {{"now", true}}, code::invalid_params);
}

// G7 review: the non-editor game plays a QUAT_SCENE from the first frame; a
// scene file read or written mid-play would mix simulated state, the play
// session's bodies and script instances with the authored scene.
TEST_F(WorldWithServices, SceneFilesWaitForStop) {
    root.write("scenes/a.json", R"({"version": 2, "entities": []})");
    ok("sim.play");
    EXPECT_TRUE(contains(err("scene.load", {{"path", "scenes/a.json"}}, code::failed), "sim.stop"));
    EXPECT_TRUE(contains(err("scene.save", {{"path", "scenes/a.json"}}, code::failed), "sim.stop"));
    EXPECT_TRUE(loaded.empty() && saved.empty()) << "nothing reached the application";
    ok("sim.stop");
    ok("scene.load", {{"path", "scenes/a.json"}});
    ok("scene.save", {{"path", "scenes/a.json"}});
    EXPECT_EQ(loaded.size(), 1U);
    EXPECT_EQ(saved.size(), 1U);
}

TEST_F(WorldWithServices, ServiceFailuresAreReported) {
    services = {}; // re-register with failing services
    services.project_root = [this] { return root.path(); };
    services.set_playing = [](bool) -> std::expected<bool, engine::core::Error> {
        return std::unexpected(engine::core::Error{"no physics in this mode"});
    };
    services.nav_bake = []() -> std::expected<std::size_t, engine::core::Error> {
        return std::unexpected(engine::core::Error{"nothing walkable"});
    };
    register_commands();
    EXPECT_TRUE(contains(err("sim.play", Json::object(), code::failed), "no physics"));
    EXPECT_TRUE(contains(err("nav.bake", Json::object(), code::failed), "nothing walkable"));
    err("camera.get", Json::object(), code::unavailable);
}

} // namespace
