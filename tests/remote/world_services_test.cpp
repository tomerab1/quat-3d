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

#include "engine/renderer/render_settings.hpp"
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
    engine::renderer::RenderSettings settings;
    engine::renderer::CloudSettings& clouds = settings.clouds;
    bool                   playing = false;
    std::vector<fs::path>  saved;
    std::vector<fs::path>  loaded;
    std::vector<fs::path>  spawned;
    int                    spawn_roots = 1;
    engine::remote::TerrainStatus terrain{.generating = false,
                                          .tiles_loaded = 1,
                                          .tiles_wanted = 1,
                                          .has_bounds = true,
                                          .bounds_min = {-50.0F, -2.0F, -50.0F},
                                          .bounds_max = {50.0F, 30.0F, 50.0F}};

    // A terrain entity the runtime has generated (regenerate consumed).
    entt::entity add_terrain(const Json& position) {
        const Json made = ok("scene.create", {{"name", "ground"},
                                              {"components",
                                               {{"transform", {{"position", position}}},
                                                {"terrain", {{"tile_size", 100}}}}}});
        const entt::entity e = entity(made["id"]);
        reg().get<s::Terrain>(e).regenerate = false;
        return e;
    }

    void SetUp() override {
        services.project_root = [this] { return root.path(); };
        services.camera_get = [this] { return camera; };
        services.camera_set = [this](const CameraPose& p) { camera = p; };
        services.render_settings = &settings;
        // Terrain tiles: height lx + 2 lz above the anchor where lx >= 0.
        services.terrain_height = [](float x, float z) -> std::optional<float> {
            if (x < 0.0F) return std::nullopt;
            return x + 2.0F * z;
        };
        services.terrain_status = [this] { return terrain; };
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

    // Without terrain the query says so (instead of answering nulls).
    std::string msg = err("world.ground_height", {{"points", {{1, 2}}}}, code::not_found);
    EXPECT_TRUE(contains(msg, "no terrain")) << msg;

    add_terrain({0, 0, 0});
    const Json h = ok("world.ground_height", {{"points", {{1, 2}, {-5, 0}}}});
    EXPECT_FLOAT_EQ(h["heights"][0].get<float>(), 5.0F);
    EXPECT_TRUE(h["heights"][1].is_null());
    EXPECT_EQ(h["generating"], false);
    Json many = Json::array();
    for (int i = 0; i < 1025; ++i) many.push_back({0, 0});
    err("world.ground_height", {{"points", many}}, code::invalid_params);
}

// Issue #39: commands later in a batch see an earlier terrain move, and a
// shape edit reports the terrain as regenerating instead of stale heights.
TEST_F(WorldWithServices, GroundHeightSeesTerrainMovesInTheSameBatch) {
    add_terrain({0, 0, 0});
    const Json r = ok("scene.batch",
                      {{"commands",
                        {{{"method", "world.ground_height"}, {"params", {{"points", {{11, 0}}}}}},
                         {{"method", "scene.update"},
                          {"params",
                           {{"entity", "ground"},
                            {"components", {{"transform", {{"position", {10, 5, 0}}}}}}}}},
                         {{"method", "world.ground_height"},
                          {"params", {{"points", {{11, 0}, {9, 0}}}}}}}}});
    ASSERT_EQ(r["results"].size(), 3U) << r.dump();
    EXPECT_FLOAT_EQ(r["results"][0]["result"]["heights"][0].get<float>(), 11.0F);
    const Json& moved = r["results"][2]["result"]["heights"];
    EXPECT_FLOAT_EQ(moved[0].get<float>(), 6.0F) << "local x 1 above the new anchor (y 5)";
    EXPECT_TRUE(moved[1].is_null()) << "x 9 is now outside the tiles";

    // A parented terrain is anchored at its world position.
    const Json parent = ok("scene.create", {{"name", "island"},
                                            {"components", {{"transform", {{"position", {0, 100, 0}}}}}}});
    ok("scene.set_parent", {{"entity", "ground"}, {"parent", parent["id"]}, {"keep_world", false}});
    EXPECT_FLOAT_EQ(ok("world.ground_height", {{"points", {{11, 0}}}})["heights"][0].get<float>(),
                    106.0F);

    // A shape edit: the loaded tiles are stale until the runtime regenerates.
    ok("scene.update", {{"entity", "ground"}, {"components", {{"terrain", {{"seed", 99}}}}}});
    const Json pending = ok("world.ground_height", {{"points", {{11, 0}}}});
    EXPECT_TRUE(pending["heights"][0].is_null());
    EXPECT_EQ(pending["generating"], true);
    // ...a shading edit is not.
    reg().get<s::Terrain>(entity(ok("scene.get", {{"entity", "ground"}})["id"])).regenerate = false;
    ok("scene.update", {{"entity", "ground"}, {"components", {{"terrain", {{"snowline", 40}}}}}});
    EXPECT_EQ(ok("world.ground_height", {{"points", {{11, 0}}}})["generating"], false);
}

TEST_F(WorldWithServices, TerrainStatusReportsTilesAndBounds) {
    EXPECT_EQ(ok("terrain.status")["present"], false);
    add_terrain({10, 5, 0});
    Json st = ok("terrain.status");
    EXPECT_EQ(st["present"], true);
    EXPECT_EQ(st["streaming"], false);
    EXPECT_EQ(st["generating"], false);
    EXPECT_EQ(st["tiles_loaded"], 1);
    EXPECT_EQ(st["tiles_wanted"], 1);
    EXPECT_FLOAT_EQ(st["anchor"][1].get<float>(), 5.0F);
    EXPECT_FLOAT_EQ(st["bounds"]["min"][0].get<float>(), -40.0F) << "anchor-relative bounds";
    EXPECT_FLOAT_EQ(st["bounds"]["max"][1].get<float>(), 35.0F);

    // A streaming window still filling in.
    ok("scene.update", {{"entity", "ground"},
                        {"components", {{"terrain", {{"streaming", true}, {"stream_radius", 1}}}}}});
    terrain.tiles_wanted = 9;
    terrain.tiles_loaded = 4;
    terrain.generating = true;
    st = ok("terrain.status");
    EXPECT_EQ(st["streaming"], true);
    EXPECT_EQ(st["generating"], true);
    EXPECT_EQ(st["tiles_loaded"], 4);
    EXPECT_EQ(st["tiles_wanted"], 9);
    err("terrain.status", {{"x", 1}}, code::invalid_params);
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

TEST_F(WorldWithServices, CameraClipPlanesAndCuts) {
    // Derived planes by default.
    Json got = ok("camera.get");
    EXPECT_EQ(got["near_auto"], true);
    EXPECT_EQ(got["far_auto"], true);
    EXPECT_FLOAT_EQ(got["far"].get<float>(), 1000.0F);

    // Explicit planes are stored as overrides; null returns to derived.
    ok("camera.set", {{"near", 0.5}, {"far", 20000}});
    ASSERT_TRUE(camera.near_override.has_value());
    EXPECT_FLOAT_EQ(*camera.near_override, 0.5F);
    EXPECT_FLOAT_EQ(*camera.far_override, 20000.0F);
    ok("camera.set", {{"far", nullptr}});
    EXPECT_FALSE(camera.far_override.has_value());
    EXPECT_TRUE(camera.near_override.has_value()) << "an omitted plane keeps its override";
    std::string msg = err("camera.set", {{"near", 10}, {"far", 5}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "far: must be greater than near")) << msg;
    msg = err("camera.set", {{"near", 0}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "near: expected a number in [0.001, 10000]")) << msg;
    err("camera.set", {{"far", "far"}}, code::invalid_params);

    // Without `cut`, small moves are continuous motion and jumps / turns are cuts.
    camera = CameraPose{{0, 2, 10}, -90.0F, 0.0F, 60.0F};
    ok("camera.set", {{"position", {0.5, 2, 10}}, {"yaw_deg", -85}});
    EXPECT_FALSE(camera.cut);
    ok("camera.set", {{"position", {0, 2, 500}}}); // > 10% of the 1000 m far plane
    EXPECT_TRUE(camera.cut);
    ok("camera.set", {{"yaw_deg", 0}}); // 90 degree turn
    EXPECT_TRUE(camera.cut);
    // An explicit flag wins either way.
    ok("camera.set", {{"position", {0, 2, -500}}, {"cut", false}});
    EXPECT_FALSE(camera.cut);
    ok("camera.set", {{"position", {0, 2, -500.1}}, {"cut", true}});
    EXPECT_TRUE(camera.cut);
    err("camera.set", {{"cut", 1}}, code::invalid_params);
    ok("camera.frame");
    EXPECT_TRUE(camera.cut) << "framing is a cut";
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
