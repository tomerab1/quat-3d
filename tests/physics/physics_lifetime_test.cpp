// Shape and character-slot lifetime in PhysicsWorld (issue #47): shapes are
// owned by the bodies that use them and released with the last one, freed
// slots are recycled, and stale handles never alias a recycled slot.

#include <cstdint>
#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/terrain/generator.hpp"
#include "physics/slot_map.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;
using engine::physics::detail::SlotMap;

constexpr float k_dt = 1.0F / 60.0F;
constexpr int   k_cycles = 200;

TEST(SlotMap, RecyclesFreedSlotsWithANewGeneration) {
    SlotMap<int> map;
    const std::uint32_t a = map.insert(1);
    const std::uint32_t b = map.insert(2);
    ASSERT_TRUE(map.erase(a));
    EXPECT_FALSE(map.contains(a));
    const std::uint32_t c = map.insert(3);
    EXPECT_EQ(c & SlotMap<int>::index_mask, a & SlotMap<int>::index_mask) << "slot not reused";
    EXPECT_NE(c, a) << "a recycled slot must get a new handle";
    EXPECT_EQ(map.get(a), nullptr) << "stale handle resolved to the new occupant";
    ASSERT_NE(map.get(c), nullptr);
    EXPECT_EQ(*map.get(c), 3);
    EXPECT_EQ(*map.get(b), 2);
    EXPECT_EQ(map.size(), 2U);
    EXPECT_EQ(map.capacity(), 2U);
    EXPECT_FALSE(map.erase(SlotMap<int>::invalid));
}

class PhysicsLifetime : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
    }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
};

TEST_F(PhysicsLifetime, StreamingATileInAndOutReleasesItsHeightField) {
    engine::terrain::TerrainParams tp;
    tp.resolution = 33;
    tp.tile_size_m = 32.0F;
    tp.erosion_droplets = 0;
    const engine::terrain::Heightmap map = engine::terrain::generate(tp);
    for (int i = 0; i < k_cycles; ++i) {
        const std::uint32_t body = s::add_height_field_body(*world, map, glm::vec3(0.0F));
        ASSERT_NE(body, PhysicsWorld::invalid_body);
        EXPECT_EQ(world->shape_count(), 1U);
        world->update(k_dt);
        world->remove_body(body); // the tile streamed out
    }
    EXPECT_EQ(world->shape_count(), 0U) << "height fields leaked";
    EXPECT_EQ(world->body_count(), 0U);
}

TEST_F(PhysicsLifetime, RebuildingAColliderReleasesTheOldShape) {
    const entt::entity e = registry.create();
    auto& t = registry.emplace<s::Transform>(e);
    t.local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 0.0F));
    t.world = t.local;
    registry.emplace<s::Collider>(e);
    registry.emplace<s::RigidBody>(e);
    s::physics_system(registry, *world, k_dt);
    ASSERT_EQ(world->shape_count(), 1U);

    for (int i = 0; i < k_cycles; ++i) {
        // A collider edit (inspector / remote scene.update) invalidates the
        // body; the next tick sweeps it and builds a new one.
        const float half = 0.25F + 0.001F * static_cast<float>(i);
        ASSERT_OK(s::apply_component(registry, e, "collider",
                                     {{"half_extents", {half, half, half}}}, true));
        s::physics_system(registry, *world, k_dt);
        EXPECT_EQ(world->shape_count(), 1U) << "cycle " << i;
    }
    EXPECT_EQ(world->body_count(), 1U);
    registry.destroy(e);
    s::physics_system(registry, *world, k_dt);
    EXPECT_EQ(world->shape_count(), 0U);
}

TEST_F(PhysicsLifetime, OneShapeMayBackSeveralBodies) {
    const std::uint32_t shape = world->create_sphere(0.5F);
    PhysicsWorld::BodyParams p;
    p.shape = shape;
    const std::uint32_t a = world->add_body(p);
    p.position = glm::vec3(3.0F, 0.0F, 0.0F);
    const std::uint32_t b = world->add_body(p);
    world->remove_body(a);
    EXPECT_EQ(world->shape_count(), 1U) << "released while a body still uses it";
    world->remove_body(b);
    EXPECT_EQ(world->shape_count(), 0U);
}

TEST_F(PhysicsLifetime, UnusedShapesAreReleasedExplicitly) {
    const std::uint32_t shape = world->create_box(glm::vec3(1.0F));
    EXPECT_EQ(world->shape_count(), 1U);
    world->release_shape(shape);
    EXPECT_EQ(world->shape_count(), 0U);
    PhysicsWorld::BodyParams p;
    p.shape = shape; // stale
    EXPECT_EQ(world->add_body(p), PhysicsWorld::invalid_body);
}

TEST_F(PhysicsLifetime, CharacterSlotsAreRecycled) {
    std::uint32_t first = PhysicsWorld::invalid_body;
    for (int i = 0; i < k_cycles; ++i) {
        const std::uint32_t c = world->create_character(0.5F, 0.3F, glm::vec3(0.0F), 7);
        ASSERT_NE(c, PhysicsWorld::invalid_body);
        if (i == 0) first = c;
        EXPECT_EQ(c & SlotMap<int>::index_mask, 0U) << "slot not reused on cycle " << i;
        world->remove_character(c);
    }
    EXPECT_EQ(world->character_count(), 0U);
    // A stale handle is a no-op, not the new occupant of its slot.
    const std::uint32_t live = world->create_character(0.5F, 0.3F, glm::vec3(4.0F), 7);
    EXPECT_NE(live, first);
    EXPECT_EQ(world->character_position(first), glm::vec3(0.0F));
    EXPECT_EQ(world->character_position(live), glm::vec3(4.0F));
}

TEST_F(PhysicsLifetime, CharacterEditsDoNotGrowTheCharacterTable) {
    const entt::entity e = registry.create();
    registry.emplace<s::Transform>(e);
    registry.emplace<s::CharacterController>(e);
    for (int i = 0; i < k_cycles; ++i) {
        ASSERT_OK(s::apply_component(registry, e, "character", {{"speed", 3.0F}}, true));
        s::character_system(registry, *world, k_dt);
        ASSERT_EQ(world->owned_characters().size(), 1U);
    }
    const std::uint32_t c = registry.get<s::CharacterController>(e).character;
    EXPECT_LT(c & SlotMap<int>::index_mask, 2U) << "character slots are not reused";
}

} // namespace
