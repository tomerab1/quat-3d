// Viewport picking (S1 audit C14): a click from inside an enclosing mesh's
// bounds (a room, a building) used to hit it at distance 0, so it won every
// click over the objects inside it.

#ifdef ENGINE_EDITOR

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "editor/picking.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/scene/components.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;
using engine::editor::pick_distance;
using engine::editor::pick_entity;

constexpr entt::entity k_none = entt::null;
const a::Aabb k_unit_box{glm::vec3(-0.5F), glm::vec3(0.5F)};

// A loaded mesh asset with only bounds (picking reads nothing else).
a::AssetHandle<a::MeshAsset> bounds_mesh(a::AssetManager& assets, const std::string& key,
                                         const a::Aabb& bounds) {
    return assets.load<a::MeshAsset>(
        key, [&](const std::filesystem::path&) -> std::expected<a::MeshAsset, engine::core::Error> {
            a::MeshAsset m;
            m.bounds = bounds;
            return m;
        });
}

entt::entity mesh_entity(entt::registry& r, const a::AssetHandle<a::MeshAsset>& mesh,
                         const glm::mat4& world) {
    const entt::entity e = r.create();
    auto& t = r.emplace<s::Transform>(e);
    t.local = world;
    t.world = world;
    r.emplace<s::MeshRenderer>(e, mesh, a::AssetHandle<a::MaterialAsset>{});
    return e;
}

TEST(EditorPicking, OutsideHitsTheNearFace) {
    const std::optional<float> d =
        pick_distance(glm::vec3(0.0F, 0.0F, -5.0F), glm::vec3(0.0F, 0.0F, 1.0F),
                      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 2.0F)), k_unit_box);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(*d, 6.5F, 1e-4F);
}

TEST(EditorPicking, InsideReportsTheExitNotZero) {
    // Camera inside a 20 m room looking +Z: the far wall is 10 m away.
    const std::optional<float> d =
        pick_distance(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F),
                      glm::scale(glm::mat4(1.0F), glm::vec3(20.0F)), k_unit_box);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(*d, 10.0F, 1e-3F);
}

TEST(EditorPicking, BoxesBehindTheRayAndDegenerateTransformsMiss) {
    EXPECT_FALSE(pick_distance(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F),
                               glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -3.0F)),
                               k_unit_box));
    EXPECT_FALSE(pick_distance(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F),
                               glm::scale(glm::mat4(1.0F), glm::vec3(0.0F)), k_unit_box));
}

TEST(EditorPicking, ObjectInsideAnEnclosingMeshWins) {
    a::AssetManager assets;
    const auto box = bounds_mesh(assets, "picking/box", k_unit_box);
    entt::registry r;
    const entt::entity room = mesh_entity(r, box, glm::scale(glm::mat4(1.0F), glm::vec3(20.0F)));
    const entt::entity crate =
        mesh_entity(r, box, glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 4.0F)));

    // Looking at the crate from inside the room.
    EXPECT_EQ(pick_entity(r, glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F)), crate);
    // Looking away from it: only the room's wall is in the way.
    EXPECT_EQ(pick_entity(r, glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F)), room);
    // Looking out of the room from outside it, away from everything.
    EXPECT_EQ(pick_entity(r, glm::vec3(0.0F, 0.0F, -50.0F), glm::vec3(0.0F, 0.0F, -1.0F)),
              k_none);
}

} // namespace

#endif // ENGINE_EDITOR
