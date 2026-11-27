// Terrain height-field bodies: add_terrain_body places Jolt's height field
// exactly under the rendered tile, so a sphere dropped through the ECS
// physics system rests on the sampled surface height.

#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/terrain/generator.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
namespace t = engine::terrain;
using engine::physics::PhysicsWorld;

[[nodiscard]] t::TerrainParams small_tile() {
    t::TerrainParams tp;
    tp.resolution = 65;
    tp.tile_size_m = 64.0F;
    tp.height_m = 8.0F;
    tp.octaves = 4;
    tp.erosion_droplets = 0;
    return tp;
}

class TerrainPhysics : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));

        // The terrain entity's translation is the tile centre; its y offsets
        // the surface.
        terrain = registry.create();
        registry.emplace<s::Terrain>(terrain, s::Terrain{params});
        auto& tf = registry.emplace<s::Transform>(terrain);
        tf.local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 2.0F, -5.0F));
        tf.world = tf.local;
    }

    const t::TerrainParams      params = small_tile();
    const t::Heightmap          map = t::generate(params);
    std::optional<PhysicsWorld> world;
    entt::registry              registry;
    entt::entity                terrain = entt::null;
};

TEST_F(TerrainPhysics, AddsABodyForTheTerrainEntity) {
    EXPECT_NE(s::add_terrain_body(registry, *world, map), PhysicsWorld::invalid_body)
        << "body creation failed";
}

TEST_F(TerrainPhysics, NoTerrainEntityAddsNoBody) {
    registry.destroy(terrain);
    EXPECT_EQ(s::add_terrain_body(registry, *world, map), PhysicsWorld::invalid_body);
}

TEST_F(TerrainPhysics, DroppedSphereRestsOnTheSurface) {
    ASSERT_NE(s::add_terrain_body(registry, *world, map), PhysicsWorld::invalid_body);

    // Drop a sphere onto the tile centre through the ECS physics system.
    constexpr float radius = 0.5F;
    const entt::entity sphere = registry.create();
    auto& st = registry.emplace<s::Transform>(sphere);
    st.local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 40.0F, -5.0F));
    st.world = st.local;
    s::Collider col;
    col.shape = s::ColliderShape::sphere;
    col.half_extents = glm::vec3(radius);
    registry.emplace<s::Collider>(sphere, col);
    s::RigidBody rb;
    rb.motion = s::BodyMotion::dynamic;
    registry.emplace<s::RigidBody>(sphere, rb);

    for (int i = 0; i < 300; ++i) s::physics_system(registry, *world, 1.0F / 60.0F);

    // The drop at (10, -5) lands at the heightmap centre; entity y (2)
    // offsets the surface.
    const float surface = 2.0F + map.sample(map.tile_size_m * 0.5F, map.tile_size_m * 0.5F);
    const float y = registry.get<s::Transform>(sphere).world[3].y;
    EXPECT_LE(y, 39.0F) << "sphere did not fall";
    EXPECT_NEAR(y, surface + radius, 1.0F) << "sphere not resting on the terrain surface";
}

} // namespace
