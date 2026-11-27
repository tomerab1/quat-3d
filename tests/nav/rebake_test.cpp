// Scene bakes and re-bakes (S1 N3/N13): bake_scene_navmesh, the navmesh
// generation counter and agents' reaction to a changed / missing mesh.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/terrain/generator.hpp"
#include "nav/nav_fixtures.hpp"

namespace {

namespace s = engine::scene;
using quat::test::add_box;
using quat::test::kNavDt;

TEST(Nav, GenerationIsUniquePerBake) {
    entt::registry r;
    add_box(r, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 10.0F));
    auto a = s::bake_scene_navmesh(r, {});
    auto b = s::bake_scene_navmesh(r, {});
    ASSERT_OK(a);
    ASSERT_OK(b);
    EXPECT_NE(a->generation(), 0U);
    EXPECT_NE(a->generation(), b->generation());
    EXPECT_EQ(engine::nav::NavMesh{}.generation(), 0U);
}

TEST(Nav, BakeWithNothingToBakeFails) {
    entt::registry r;
    EXPECT_ERR(s::bake_scene_navmesh(r, {}));
}

// Regression (S1 N3): the bake used the retained heightmap even after the
// Terrain entity was deleted — the frame loop now passes tiles only while a
// terrain exists; without tiles, nothing of the terrain is baked.
TEST(Nav, BakeIncludesTerrainOnlyWhenGiven) {
    engine::terrain::TerrainParams tp;
    tp.resolution = 65;
    tp.tile_size_m = 64.0F;
    tp.height_m = 2.0F;
    tp.octaves = 2;
    tp.erosion_droplets = 0;
    const engine::terrain::Heightmap map = engine::terrain::generate(tp);

    entt::registry r;
    add_box(r, glm::vec3(100.0F, -0.5F, 0.0F), glm::vec3(5.0F, 0.5F, 5.0F)); // far from the tile
    const s::NavTerrainTile tile{&map, glm::vec3(0.0F)};
    auto with = s::bake_scene_navmesh(r, std::span(&tile, 1));
    ASSERT_OK(with);
    EXPECT_OK(with->nearest_point({0.0F, map.max_height, 0.0F}));
    auto without = s::bake_scene_navmesh(r, {});
    ASSERT_OK(without);
    EXPECT_ERR(without->nearest_point({0.0F, map.max_height, 0.0F}));
}

// Regression (S1 N3): agents kept walking their old path after a re-bake.
TEST(Nav, AgentsReplanAfterARebake) {
    quat::test::NavWorld w;
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    w.bake();
    const entt::entity e = w.agent({-10.0F, 0.0F, 0.0F});
    s::nav_agent_set_target(w.nav(e), {10.0F, 0.0F, 0.0F});
    w.run(5, [] { return false; });
    ASSERT_EQ(w.nav(e).path.size(), 2U) << "straight across the open floor";

    // A wall appears across the route; the editor re-bakes.
    add_box(w.registry, glm::vec3(0.0F, 1.5F, 0.0F), glm::vec3(0.5F, 1.5F, 8.0F));
    w.bake();
    w.run(1, [] { return false; });
    EXPECT_GE(w.nav(e).path.size(), 3U) << "the stale straight path was kept";
    EXPECT_EQ(w.nav(e).planned_generation, w.navmesh.generation());
    w.run(60 * 20, [&] { return !w.nav(e).active; });
    EXPECT_TRUE(w.nav(e).arrived);
}

TEST(Nav, AgentsHoldStillWithoutANavmesh) {
    quat::test::NavWorld w;
    const entt::entity e = w.agent({0.0F, 1.0F, 0.0F});
    auto& cc = w.registry.emplace<s::CharacterController>(e);
    cc.move = glm::vec3(1.0F, 0.0F, 0.0F); // left over from the previous mesh
    s::nav_agent_set_target(w.nav(e), {10.0F, 0.0F, 0.0F});
    ASSERT_FALSE(w.navmesh.valid());
    s::nav_agent_system(w.registry, w.navmesh, kNavDt);
    EXPECT_EQ(cc.move, glm::vec3(0.0F));
    EXPECT_TRUE(w.nav(e).active) << "the order survives until a mesh exists";
}

} // namespace
