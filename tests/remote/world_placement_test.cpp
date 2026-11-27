// Entity bounds in scene.get / scene.list and world.place_on_ground (issue
// #40), against fake terrain and physics services.

#include <cmath>
#include <optional>
#include <string>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "remote/world_fixture.hpp"
#include "support/expect_ok.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;
using engine::physics::PhysicsWorld;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void expect_vec3(const Json& v, const glm::vec3& want, float tol = 1e-3F) {
    ASSERT_TRUE(v.is_array() && v.size() == 3) << v.dump();
    EXPECT_NEAR(v[0].get<float>(), want.x, tol) << v.dump();
    EXPECT_NEAR(v[1].get<float>(), want.y, tol) << v.dump();
    EXPECT_NEAR(v[2].get<float>(), want.z, tol) << v.dump();
}

class Placement : public quat::test::WorldCommands {
protected:
    std::optional<PhysicsWorld> physics;

    void SetUp() override {
        // Terrain tiles (within 100 m of the anchor): a slope rising 0.1 m per
        // metre along +x.
        services.terrain_height = [](float x, float z) -> std::optional<float> {
            if (std::abs(x) > 100.0F || std::abs(z) > 100.0F) return std::nullopt;
            return 0.1F * x;
        };
        services.terrain_status = [] {
            return engine::remote::TerrainStatus{.generating = false,
                                                 .tiles_loaded = 1,
                                                 .tiles_wanted = 1,
                                                 .has_bounds = true,
                                                 .bounds_min = {-100.0F, -10.0F, -100.0F},
                                                 .bounds_max = {100.0F, 10.0F, 100.0F}};
        };
        services.physics_world = [this]() -> const PhysicsWorld* {
            return physics ? &*physics : nullptr;
        };
        register_commands();
    }

    // The terrain, anchored 2 m up (ground y = 2 + 0.1 x).
    void add_terrain() {
        const Json t = ok("scene.create", {{"name", "ground"},
                                           {"components",
                                            {{"transform", {{"position", {0, 2, 0}}}},
                                             {"terrain", {{"tile_size", 200}}}}}});
        reg().get<s::Terrain>(entity(t["id"])).regenerate = false;
    }

    Json create(const Json& params) { return ok("scene.create", params); }

    [[nodiscard]] glm::mat4 world_of(const std::string& name) {
        return reg().get<s::Transform>(entity(ok("scene.get", {{"entity", name}})["id"])).world;
    }
};

TEST_F(Placement, BoundsFollowRotationScaleAndChildren) {
    create({{"name", "crate"},
            {"components",
             {{"transform", {{"position", {10, 1, 0}}, {"rotation", {0, 90, 0}}, {"scale", {2, 1, 4}}}},
              {"primitive", {{"shape", "cube"}}}}}});
    Json got = ok("scene.get", {{"entity", "crate"}});
    expect_vec3(got["world_bounds"]["min"], {8.0F, 0.5F, -1.0F});
    expect_vec3(got["world_bounds"]["max"], {12.0F, 1.5F, 1.0F});
    expect_vec3(got["local_bounds"]["min"], glm::vec3(-0.5F));

    // A group: an empty parent's bounds cover its children.
    const Json group = create({{"name", "group"},
                               {"components", {{"transform", {{"position", {0, 0, 5}}}}}}});
    create({{"name", "ball"},
            {"parent", group["id"]},
            {"components",
             {{"transform", {{"position", {3, 0, 0}}, {"scale", 2}}}, {"primitive", {{"shape", "sphere"}}}}}});
    got = ok("scene.get", {{"entity", "group"}});
    expect_vec3(got["world_bounds"]["min"], {2.0F, -1.0F, 4.0F});
    expect_vec3(got["world_bounds"]["max"], {4.0F, 1.0F, 6.0F});
    expect_vec3(got["local_bounds"]["min"], {2.0F, -1.0F, -1.0F});

    // scene.list carries them too; an empty draws nothing.
    create({{"name", "marker"}});
    const Json list = ok("scene.list");
    for (const Json& e : list["entities"]) {
        if (e["name"] == "marker") EXPECT_TRUE(e["world_bounds"].is_null());
        if (e["name"] == "crate") expect_vec3(e["world_bounds"]["max"], {12.0F, 1.5F, 1.0F});
    }

    // The terrain entity reports its loaded tiles.
    add_terrain();
    got = ok("scene.get", {{"entity", "ground"}});
    expect_vec3(got["world_bounds"]["min"], {-100.0F, -8.0F, -100.0F});
    EXPECT_TRUE(got["local_bounds"].is_null());
}

TEST_F(Placement, RestsOnTerrain) {
    add_terrain();
    create({{"name", "rock"},
            {"components",
             {{"transform", {{"position", {10, 50, 0}}}}, {"primitive", {{"shape", "cube"}}}}}});
    // Unaligned: the lowest ground under the footprint (x 9.5..10.5 -> 2.95).
    Json r = ok("world.place_on_ground", {{"entity", "rock"}});
    expect_vec3(r["position"], {10.0F, 3.45F, 0.0F});
    EXPECT_NEAR(r["world_bounds"]["min"][1].get<float>(), 2.95F, 1e-3F);
    EXPECT_EQ(r["surface"]["kind"], "terrain");
    EXPECT_NEAR(r["tilt_deg"].get<float>(), 0.0F, 1e-4F);

    // `at` moves it first; `offset` sinks it.
    r = ok("world.place_on_ground", {{"entity", "rock"}, {"at", {-20, 4}}, {"offset", -0.2}});
    expect_vec3(r["position"], {-20.0F, 2.0F - 2.05F + 0.5F - 0.2F, 4.0F});

    // Aligned to the slope: tilted by atan(0.1) about z, the bottom centre on the ground.
    r = ok("world.place_on_ground", {{"entity", "rock"}, {"at", {10, 0}}, {"align_to_normal", true}});
    const float slope_deg = glm::degrees(std::atan(0.1F));
    EXPECT_NEAR(r["tilt_deg"].get<float>(), slope_deg, 0.05F);
    glm::mat4 w = world_of("rock");
    const glm::vec3 up = glm::normalize(glm::vec3(w[1]));
    EXPECT_NEAR(glm::dot(up, glm::normalize(glm::vec3(-0.1F, 1.0F, 0.0F))), 1.0F, 1e-4F);
    const glm::vec3 bottom(w * glm::vec4(0.0F, -0.5F, 0.0F, 1.0F));
    EXPECT_NEAR(bottom.y, 2.0F + 0.1F * bottom.x, 1e-3F);

    // Re-aligning is idempotent; the tilt can be capped.
    r = ok("world.place_on_ground", {{"entity", "rock"}, {"align_to_normal", true}});
    EXPECT_NEAR(r["tilt_deg"].get<float>(), slope_deg, 0.05F);
    ok("scene.update", {{"entity", "rock"}, {"components", {{"transform", {{"rotation", {0, 0, 0}}}}}}});
    r = ok("world.place_on_ground",
           {{"entity", "rock"}, {"align_to_normal", true}, {"max_tilt_deg", 2}});
    EXPECT_NEAR(r["tilt_deg"].get<float>(), 2.0F, 0.05F);
}

TEST_F(Placement, RestsOnCollidersInEditMode) {
    // A table: a static box collider whose top is at y = 1.5, spanning x -42..-38.
    create({{"name", "table"},
            {"components",
             {{"transform", {{"position", {-40, 1, 0}}}},
              {"collider", {{"shape", "box"}, {"half_extents", {2, 0.5, 2}}}}}}});
    // A crate with its own collider (ignored: it does not rest on itself).
    create({{"name", "crate"},
            {"components",
             {{"transform", {{"position", {-40, 10, 0}}}},
              {"primitive", {{"shape", "cube"}}},
              {"collider", {{"shape", "box"}}}}}});
    Json r = ok("world.place_on_ground", {{"entity", "crate"}});
    expect_vec3(r["position"], {-40.0F, 2.0F, 0.0F});
    EXPECT_EQ(r["surface"]["kind"], "collider");
    EXPECT_EQ(r["surface"]["entity"], ok("scene.get", {{"entity", "table"}})["id"]);

    // Overhanging the edge it stays on the table, though the ground (about
    // -1.9 m there) is below the overhang.
    add_terrain();
    r = ok("world.place_on_ground", {{"entity", "crate"}, {"at", {-38.2, 0}}});
    EXPECT_NEAR(r["position"][1].get<float>(), 2.0F, 1e-3F);
    // Off the table entirely: the terrain (lowest under x -30.5..-29.5).
    r = ok("world.place_on_ground", {{"entity", "crate"}, {"at", {-30, 0}}});
    EXPECT_NEAR(r["position"][1].get<float>(), 2.0F - 3.05F + 0.5F, 1e-3F);
    EXPECT_EQ(r["surface"]["kind"], "terrain");
    // A sphere collider is a surface too (its top).
    create({{"name", "boulder"},
            {"components",
             {{"transform", {{"position", {-20, 0, 0}}, {"scale", 2}}},
              {"collider", {{"shape", "sphere"}, {"half_extents", {1, 1, 1}}}}}}});
    // (Surfaces count below the top of the entity's bounds: lift it first.)
    ok("scene.update", {{"entity", "crate"},
                        {"components", {{"transform", {{"position", {-20, 10, 0}}}}}}});
    r = ok("world.place_on_ground", {{"entity", "crate"}});
    EXPECT_NEAR(r["position"][1].get<float>(), 2.5F, 1e-3F) << "on the 2 m sphere's top";
}

TEST_F(Placement, RestsOnPhysicsBodies) {
    auto world = PhysicsWorld::create();
    ASSERT_OK(world);
    physics = std::move(*world);
    const Json box = create({{"name", "pillar"}});
    PhysicsWorld::BodyParams body;
    body.shape = physics->create_box(glm::vec3(1.0F));
    body.position = glm::vec3(30.0F, 0.0F, 0.0F);
    body.motion = engine::physics::Motion::static_body;
    body.layer = engine::physics::Layer::static_body;
    body.owner = box["id"].get<std::uint32_t>();
    ASSERT_NE(physics->add_body(body), PhysicsWorld::invalid_body);

    create({{"name", "ball"},
            {"components",
             {{"transform", {{"position", {0, 20, 0}}}}, {"primitive", {{"shape", "sphere"}}}}}});
    const Json r = ok("world.place_on_ground", {{"entity", "ball"}, {"at", {30, 0.3}}});
    expect_vec3(r["position"], {30.0F, 1.5F, 0.3F});
    EXPECT_EQ(r["surface"]["kind"], "body");
    EXPECT_EQ(r["surface"]["entity"], box["id"]);
    expect_vec3(r["surface"]["normal"], {0.0F, 1.0F, 0.0F});
}

TEST_F(Placement, ErrorsSayWhatToDo) {
    create({{"name", "lamp"},
            {"components", {{"transform", {{"position", {0, 5, 0}}}}, {"primitive", {{"shape", "cone"}}}}}});
    std::string msg = err("world.place_on_ground", {{"entity", "lamp"}}, code::failed);
    EXPECT_TRUE(contains(msg, "nothing to rest on")) << msg;
    err("world.place_on_ground", {{"entity", "ghost"}}, code::not_found);
    err("world.place_on_ground", Json::object(), code::invalid_params);
    msg = err("world.place_on_ground", {{"entity", "lamp"}, {"at", {1}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "at: expected [x, z]")) << msg;
    err("world.place_on_ground", {{"entity", "lamp"}, {"max_tilt_deg", 120}}, code::invalid_params);
    add_terrain();
    msg = err("world.place_on_ground", {{"entity", "ground"}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "terrain")) << msg;
}

} // namespace
