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

namespace {

// G19.1: a terrain edit rewrites the height field in place; rays see the new
// ground and the untouched texels keep theirs.
TEST(TerrainPhysicsEdits, RegionUpdateMovesTheGroundInPlace) {
    auto w = PhysicsWorld::create();
    ASSERT_OK(w);
    PhysicsWorld world = std::move(*w);
    t::Heightmap map = t::generate(small_tile());
    ASSERT_TRUE(map.valid());
    const glm::vec3 origin(-32.0F, 0.0F, -32.0F); // tile centred on the world origin
    const std::uint32_t body = s::add_height_field_body(world, map, origin);
    ASSERT_NE(body, PhysicsWorld::invalid_body);
    const std::uint32_t shapes = world.shape_count();

    const auto ground_at = [&](float x, float z) -> std::optional<float> {
        const auto hit = world.cast_ray(glm::vec3(x, 500.0F, z), glm::vec3(0, -1, 0), 1000.0F);
        if (!hit) return std::nullopt;
        return hit->point.y;
    };
    const float far_before = ground_at(25.0F, 25.0F).value_or(-1e9F);

    // Flatten a plinth at 20 m around the tile centre (texel = 1 m).
    t::TerrainStamp plinth;
    plinth.shape = t::StampShape::polygon;
    plinth.points = {{-6, -6}, {6, -6}, {6, 6}, {-6, 6}};
    plinth.falloff = 3.0F;
    plinth.op = t::StampOp::flatten;
    plinth.height = 20.0F;
    const t::TileFrame frame{{0, 0}, map.resolution, map.tile_size_m};
    const t::TexelRect done = t::apply_edits(map, frame, std::vector<t::TerrainStamp>{plinth});
    ASSERT_FALSE(done.empty());
    ASSERT_TRUE(s::update_height_field_body(world, body, map, done));
    EXPECT_EQ(world.shape_count(), shapes); // in place, no new shape

    const std::optional<float> centre = ground_at(0.5F, 0.5F);
    ASSERT_TRUE(centre);
    EXPECT_NEAR(*centre, 20.0F, 0.05F);
    EXPECT_NEAR(ground_at(25.0F, 25.0F).value_or(1e9F), far_before, 1e-3F);

    // Beyond the reserved headroom: refused, nothing changed.
    t::TerrainStamp tower = plinth;
    tower.height = map.max_height + s::height_field_edit_headroom_m + 50.0F;
    const t::TexelRect again = t::apply_edits(map, frame, std::vector<t::TerrainStamp>{tower});
    EXPECT_FALSE(s::update_height_field_body(world, body, map, again));
    EXPECT_NEAR(ground_at(0.5F, 0.5F).value_or(-1e9F), 20.0F, 0.05F);
}

} // namespace
