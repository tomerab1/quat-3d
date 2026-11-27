// Play-mode snapshot (capture on Play, restore on Stop).

#ifdef ENGINE_EDITOR

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "editor/scene_snapshot.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

namespace s = engine::scene;

// Audit B12/C9: Stop used to drop NetReplicated (replication silently ended
// after one Play/Stop cycle); Primitive must survive the same way.
TEST(PlayModeSnapshot, RestoresNetReplicatedAndPrimitive) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("crate");
    scene.registry().emplace<s::NetReplicated>(e, 7U);
    scene.registry().emplace<s::Primitive>(e).shape = s::PrimitiveShape::cone;

    const auto snapshot = engine::editor::capture_scene(scene);
    scene.registry().remove<s::NetReplicated, s::Primitive>(e);
    engine::editor::restore_scene(scene, snapshot);

    ASSERT_TRUE(scene.registry().valid(e));
    ASSERT_TRUE(scene.registry().all_of<s::NetReplicated>(e));
    EXPECT_EQ(scene.registry().get<s::NetReplicated>(e).net_id, 7U);
    ASSERT_TRUE(scene.registry().all_of<s::Primitive>(e));
    EXPECT_EQ(scene.registry().get<s::Primitive>(e).shape, s::PrimitiveShape::cone);
}

} // namespace

#endif // ENGINE_EDITOR
