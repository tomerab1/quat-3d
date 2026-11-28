// G19.1: the Lua `terrain.edit` bindings (the same surface as the remote
// terrain.edit.* commands): world-space stamps in, ids out, precise errors.

#include <glm/gtc/matrix_transform.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/terrain_edits.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using namespace engine::scene;

TEST_F(ScriptTest, TerrainEditsFromLua) {
    const entt::entity t = scene.create_entity("terrain");
    scene.registry().emplace<Terrain>(t);
    auto& tf = scene.registry().get<Transform>(t);
    tf.local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 3.0F, 0.0F));
    tf.world = tf.local;
    write("builder.lua", R"(
        function on_start(self)
            local plinth = terrain.edit.add({ shape = "polygon",
                points = { {0, 0}, {6, 0}, {6, 4}, {0, 4} }, radius = 1, falloff = 2,
                op = "flatten", height = 10 })
            local path = terrain.edit.add({ shape = "capsule", points = { {-4, 2}, {12, 2} },
                radius = 1.2, op = "paint", channel = "grass", value = 0 }, plinth)
            terrain.edit.update(plinth, { height = 11 })
            local list = terrain.edit.list()
            print("count", #list, list[1].id == path, list[2].height, list[2].shape)
            local ok, err = pcall(terrain.edit.add, { shape = "blob", points = { {0, 0} } })
            print("bad", ok, err)
            ok, err = pcall(terrain.edit.add, { points = { {0, 0} }, radius = 2, depth = 3 })
            print("unknown", ok, err)
            print("removed", terrain.edit.remove(path), terrain.edit.remove(path))
            print("cleared", terrain.edit.clear())
        end
    )");
    (void)scripted("builder", "builder.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "count\t2\ttrue\t11.0\tpolygon")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "shape: expected one of circle, capsule, polygon"));
    EXPECT_TRUE(has_line(log, "depth: unknown field"));
    EXPECT_TRUE(has_line(log, "removed\ttrue\tfalse"));
    EXPECT_TRUE(has_line(log, "cleared\t1"));
    EXPECT_TRUE(scene.registry().get<TerrainEditLayer>(t).list.stamps().empty());
}

} // namespace
} // namespace quat::test
