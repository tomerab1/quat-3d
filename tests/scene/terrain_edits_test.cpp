// Terrain edits in the scene (G19.1): the terrain_edits codec and the
// world-space TerrainEdits service.

#include <gtest/gtest.h>

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/terrain_edits.hpp"

namespace {

namespace s = engine::scene;
namespace t = engine::terrain;
using json = nlohmann::json;

entt::entity add_terrain(entt::registry& r, glm::vec3 at) {
    const entt::entity e = r.create();
    r.emplace<s::Terrain>(e);
    auto& tf = r.emplace<s::Transform>(e);
    tf.local = glm::translate(glm::mat4(1.0F), at);
    tf.world = tf.local;
    return e;
}

t::TerrainStamp plinth_stamp() {
    t::TerrainStamp st;
    st.shape = t::StampShape::polygon;
    st.points = {{10, 10}, {20, 10}, {20, 18}, {10, 18}};
    st.radius = 1.0F;
    st.falloff = 2.0F;
    st.op = t::StampOp::flatten;
    st.height = 12.0F;
    st.pivot = {15, 14};
    return st;
}

TEST(TerrainEditsCodec, RoundTripsEveryOp) {
    entt::registry r;
    const entt::entity e = add_terrain(r, glm::vec3(0.0F));
    s::TerrainEditLayer layer;
    ASSERT_TRUE(layer.list.add(plinth_stamp()));
    t::TerrainStamp path;
    path.shape = t::StampShape::capsule;
    path.points = {{0, 0}, {5, 5}, {9, 3}};
    path.radius = 1.5F;
    path.op = t::StampOp::paint;
    path.channel = t::PaintChannel::grass;
    path.value = 0.0F;
    ASSERT_TRUE(layer.list.add(path));
    t::TerrainStamp blur = path;
    blur.op = t::StampOp::smooth;
    blur.iterations = 3;
    ASSERT_TRUE(layer.list.add(blur));
    t::TerrainStamp dig;
    dig.points = {{3, 3}};
    dig.radius = 2.0F;
    dig.op = t::StampOp::lower;
    dig.amount = 0.5F;
    ASSERT_TRUE(layer.list.add(dig));
    r.emplace<s::TerrainEditLayer>(e, layer);

    const json encoded = s::encode_components(r, e);
    ASSERT_TRUE(encoded.contains("terrain_edits"));
    entt::registry r2;
    const entt::entity e2 = add_terrain(r2, glm::vec3(0.0F));
    ASSERT_TRUE(s::apply_component(r2, e2, "terrain_edits", encoded["terrain_edits"], true));
    const auto& got = r2.get<s::TerrainEditLayer>(e2).list;
    EXPECT_EQ(got.stamps(), layer.list.stamps());
    EXPECT_EQ(got.next_id(), layer.list.next_id());
}

TEST(TerrainEditsCodec, RejectsBadStampsWithAPath) {
    entt::registry r;
    const entt::entity e = add_terrain(r, glm::vec3(0.0F));
    const json bad = {{"stamps",
                       json::array({{{"shape", "circle"},
                                     {"points", json::array({json::array({0, 0})})},
                                     {"radius", 2},
                                     {"op", "paint"},
                                     {"channel", "lava"}}})}};
    auto res = s::apply_component(r, e, "terrain_edits", bad, true);
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("channel"), std::string::npos) << res.error().message;
    EXPECT_NE(res.error().message.find("stamps[0]"), std::string::npos) << res.error().message;

    const json unknown = {{"stamps", json::array({{{"shape", "circle"},
                                                   {"points", json::array({json::array({0, 0})})},
                                                   {"radius", 2},
                                                   {"colour", 1}}})}};
    EXPECT_FALSE(s::apply_component(r, e, "terrain_edits", unknown, true));
    // Lenient (scene files) tolerates the unknown field.
    EXPECT_TRUE(s::apply_component(r, e, "terrain_edits", unknown, false));
    EXPECT_EQ(r.get<s::TerrainEditLayer>(e).list.stamps().size(), 1U);
    EXPECT_EQ(r.get<s::TerrainEditLayer>(e).list.stamps()[0].id, 1U); // id assigned
}

TEST(TerrainEditsService, WorldSpaceInLocalStored) {
    entt::registry r;
    s::TerrainEdits edits(r);
    EXPECT_FALSE(edits.add(plinth_stamp())); // no terrain yet
    const entt::entity e = add_terrain(r, glm::vec3(100.0F, 5.0F, -50.0F));

    const auto id = edits.add(plinth_stamp());
    ASSERT_TRUE(id) << id.error().message;
    const t::TerrainStamp& local = r.get<s::TerrainEditLayer>(e).list.stamps().at(0);
    EXPECT_EQ(local.points[0], glm::vec2(-90.0F, 60.0F));
    EXPECT_EQ(local.height, 7.0F);
    const auto back = edits.get(*id);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->points, plinth_stamp().points);
    EXPECT_EQ(back->height, 12.0F);

    const std::uint64_t r0 = edits.revision();
    t::TerrainStamp moved = plinth_stamp();
    moved.height = 13.0F;
    ASSERT_TRUE(edits.update(*id, moved));
    EXPECT_NE(edits.revision(), r0);
    EXPECT_EQ(edits.list().at(0).height, 13.0F);

    t::TerrainStamp broken = plinth_stamp();
    broken.points.resize(2);
    const auto err = edits.add(broken);
    ASSERT_FALSE(err);
    EXPECT_NE(err.error().message.find("points"), std::string::npos);

    EXPECT_TRUE(edits.remove(*id));
    EXPECT_FALSE(edits.remove(*id));
    ASSERT_TRUE(edits.add(plinth_stamp()));
    EXPECT_EQ(edits.clear(), 1U);
    EXPECT_TRUE(edits.list().empty());
}

} // namespace
