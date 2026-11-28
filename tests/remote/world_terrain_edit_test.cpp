// terrain.edit.* (G19.1): add / update / remove / list / clear in world
// space, precise errors for models to self-correct from, and the edit layer
// the runtime applies landing on the Terrain entity in terrain-local space.

#include <glm/gtc/matrix_transform.hpp>

#include "engine/remote/rpc_server.hpp"
#include "engine/scene/terrain_edits.hpp"
#include "remote/world_fixture.hpp"

namespace {

using quat::test::Json;
namespace rc = engine::remote::rpc_code;
namespace s = engine::scene;

class TerrainEditCommands : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
    entt::entity add_terrain(glm::vec3 at) {
        const entt::entity e = scene.create_entity("terrain");
        reg().emplace<s::Terrain>(e);
        auto& tf = reg().get<s::Transform>(e);
        tf.local = glm::translate(glm::mat4(1.0F), at);
        tf.world = tf.local;
        return e;
    }
};

const Json k_plinth = {{"shape", "polygon"},
                       {"points", Json::array({Json::array({0, 0}), Json::array({8, 0}),
                                               Json::array({8, 6}), Json::array({0, 6})})},
                       {"radius", 1},
                       {"falloff", 2},
                       {"op", "flatten"},
                       {"height", 14}};

TEST_F(TerrainEditCommands, AddListUpdateRemoveClear) {
    EXPECT_NE(err("terrain.edit.add", k_plinth, rc::not_found).find("no terrain"),
              std::string::npos);
    const entt::entity t = add_terrain(glm::vec3(10.0F, 2.0F, 0.0F));

    const Json added = ok("terrain.edit.add", k_plinth);
    ASSERT_TRUE(added.contains("id"));
    const auto id = added["id"].get<std::uint64_t>();
    EXPECT_EQ(added["count"], 1);

    Json path = {{"shape", "capsule"},
                 {"points", Json::array({Json::array({-5, 3}), Json::array({20, 3})})},
                 {"radius", 1.5},
                 {"op", "paint"},
                 {"channel", "dirt"},
                 {"before", id}};
    const auto path_id = ok("terrain.edit.add", path)["id"].get<std::uint64_t>();

    // Stored terrain-local, listed in world space, in order.
    const auto& local = reg().get<s::TerrainEditLayer>(t).list.stamps();
    ASSERT_EQ(local.size(), 2U);
    EXPECT_EQ(local[0].id, path_id);
    EXPECT_EQ(local[1].points[0], glm::vec2(-10.0F, 0.0F));
    EXPECT_EQ(local[1].height, 12.0F);
    const Json listed = ok("terrain.edit.list");
    ASSERT_EQ(listed["stamps"].size(), 2U);
    EXPECT_EQ(listed["stamps"][1]["points"][0][0], 0.0);
    EXPECT_EQ(listed["stamps"][1]["height"], 14.0);
    EXPECT_EQ(listed["stamps"][0]["channel"], "dirt");

    const auto r0 = listed["revision"].get<std::uint64_t>();
    const Json upd = ok("terrain.edit.update", {{"id", id}, {"height", 15}});
    EXPECT_NE(upd["revision"].get<std::uint64_t>(), r0);
    EXPECT_EQ(ok("terrain.edit.list")["stamps"][1]["height"], 15.0);
    EXPECT_EQ(ok("terrain.edit.list")["stamps"][1]["shape"], "polygon"); // other fields kept

    const Json removed = ok("terrain.edit.remove", {{"ids", Json::array({path_id, 999})}});
    EXPECT_EQ(removed["removed"], Json::array({path_id}));
    EXPECT_EQ(removed["missing"], Json::array({999}));
    EXPECT_EQ(ok("terrain.edit.clear")["removed"], 1);
    EXPECT_EQ(ok("terrain.edit.list")["count"], 0);
}

TEST_F(TerrainEditCommands, ErrorsNameTheField) {
    add_terrain(glm::vec3(0.0F));
    Json bad = k_plinth;
    bad["op"] = "dig";
    EXPECT_NE(err("terrain.edit.add", bad, rc::invalid_params).find("op"), std::string::npos);
    bad = k_plinth;
    bad["points"] = Json::array({Json::array({0, 0}), Json::array({1, 1})});
    EXPECT_NE(err("terrain.edit.add", bad, rc::invalid_params).find("points"), std::string::npos);
    bad = k_plinth;
    bad["colour"] = "red";
    EXPECT_NE(err("terrain.edit.add", bad, rc::invalid_params).find("colour"), std::string::npos);
    bad = k_plinth;
    bad["id"] = 4;
    EXPECT_NE(err("terrain.edit.add", bad, rc::invalid_params).find("id"), std::string::npos);
    EXPECT_NE(err("terrain.edit.update", {{"id", 77}, {"height", 1}}, rc::not_found).find("77"),
              std::string::npos);
    const auto id = ok("terrain.edit.add", k_plinth)["id"];
    EXPECT_NE(err("terrain.edit.update", {{"id", id}, {"strength", 3}}, rc::invalid_params)
                  .find("strength"),
              std::string::npos);
    EXPECT_FALSE(call("terrain.edit.remove", Json::object()));
}

} // namespace
