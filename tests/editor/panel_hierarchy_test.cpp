// Hierarchy / asset browser logic (ROADMAP E8.3): type icons, the shared
// create / delete actions behind menus, palette and context menus, and the
// asset browser's file classification.

#ifdef ENGINE_EDITOR

#include <filesystem>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "editor/asset_browser.hpp"
#include "editor/scene_hierarchy.hpp"
#include "editor/shell/scene_actions.hpp"
#include "editor/ui/icons.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

namespace ed = engine::editor;
namespace sh = engine::editor::shell;
namespace s = engine::scene;
namespace icon = engine::editor::icon;
constexpr entt::entity k_none = entt::null;

const sh::CreateKind& kind(std::string_view id) {
    for (const sh::CreateKind& k : sh::create_kinds()) {
        if (k.id == id) return k;
    }
    ADD_FAILURE() << "no create kind " << id;
    return sh::create_kinds()[0];
}

TEST(EditorHierarchy, TypeIconsFollowTheComponents) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity empty = scene.create_entity("empty");
    EXPECT_STREQ(ed::entity_type_icon(r, empty), icon::circle_dot);
    const entt::entity cam = scene.create_entity("cam");
    r.emplace<s::Camera>(cam);
    EXPECT_STREQ(ed::entity_type_icon(r, cam), icon::camera);
    const entt::entity sun = scene.create_entity("sun");
    r.emplace<s::DirectionalLight>(sun);
    EXPECT_STREQ(ed::entity_type_icon(r, sun), icon::sun);
    const entt::entity group = scene.create_entity("group");
    const entt::entity child = scene.create_entity("child");
    ASSERT_TRUE(scene.set_parent(child, group));
    EXPECT_STREQ(ed::entity_type_icon(r, group), icon::folder) << "an empty parent is a group";
    // A light wins over the mesh it may also carry (what the entity *is*).
    r.emplace<s::PointLight>(child);
    r.emplace<s::MeshRenderer>(child);
    EXPECT_STREQ(ed::entity_type_icon(r, child), icon::lightbulb);
}

TEST(EditorHierarchy, CreateKindsAddTheirComponentAtThePoint) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity light = sh::create_entity_of(scene, kind("point_light"), k_none,
                                                    glm::vec3(1.0F, 2.0F, 3.0F));
    ASSERT_NE(light, k_none);
    EXPECT_TRUE(r.all_of<s::PointLight>(light));
    EXPECT_EQ(r.get<s::Name>(light).value, "point_light");
    EXPECT_EQ(glm::vec3(r.get<s::Transform>(light).local[3]), glm::vec3(1.0F, 2.0F, 3.0F));

    const entt::entity cube = sh::create_entity_of(scene, kind("cube"), k_none, glm::vec3(0.0F));
    ASSERT_NE(cube, k_none);
    ASSERT_TRUE(r.all_of<s::Primitive>(cube));
    EXPECT_EQ(r.get<s::Primitive>(cube).shape, s::PrimitiveShape::cube);

    // Under a parent, the world point is converted into the parent's space.
    const entt::entity parent = scene.create_entity("parent");
    r.get<s::Transform>(parent).local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, 0.0F));
    scene.tick(0.0F);
    const entt::entity child =
        sh::create_entity_of(scene, kind("empty"), parent, glm::vec3(12.0F, 0.0F, 0.0F));
    ASSERT_NE(child, k_none);
    EXPECT_EQ(r.get<s::Parent>(child).entity, parent);
    EXPECT_FLOAT_EQ(r.get<s::Transform>(child).local[3].x, 2.0F);
    for (const sh::CreateKind& k : sh::create_kinds()) {
        EXPECT_NE(sh::create_entity_of(scene, k, k_none, glm::vec3(0.0F)), k_none) << k.id;
    }
}

TEST(EditorHierarchy, DeleteRemovesTheSubtreeAndTheSelection) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity root = scene.create_entity("root");
    const entt::entity a = scene.create_entity("a");
    const entt::entity b = scene.create_entity("b");
    const entt::entity keep = scene.create_entity("keep");
    ASSERT_TRUE(scene.set_parent(a, root));
    ASSERT_TRUE(scene.set_parent(b, a));
    entt::entity selected = b;
    sh::delete_entity_tree(scene, root, selected);
    EXPECT_FALSE(r.valid(root));
    EXPECT_FALSE(r.valid(a));
    EXPECT_FALSE(r.valid(b));
    EXPECT_TRUE(r.valid(keep));
    EXPECT_EQ(selected, k_none);
    sh::delete_entity_tree(scene, root, selected); // stale id: no-op
}

TEST(EditorHierarchy, SpawnPointIsInFrontOfTheCamera) {
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0F, 1.0F, 10.0F), glm::vec3(0.0F, 1.0F, 0.0F),
                                       glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec3 p = sh::spawn_point(view, 5.0F);
    EXPECT_NEAR(p.x, 0.0F, 1e-4F);
    EXPECT_NEAR(p.y, 1.0F, 1e-4F);
    EXPECT_NEAR(p.z, 5.0F, 1e-4F);
}

TEST(EditorAssetBrowser, ClassifiesFilesByExtension) {
    using ed::AssetKind;
    EXPECT_EQ(ed::asset_type("a/b", true).kind, AssetKind::folder);
    EXPECT_EQ(ed::asset_type("m.glb", false).kind, AssetKind::model);
    EXPECT_EQ(ed::asset_type("m.GLTF", false).kind, AssetKind::model) << "case-insensitive";
    EXPECT_EQ(ed::asset_type("t.png", false).kind, AssetKind::texture);
    EXPECT_EQ(ed::asset_type("t.hdr", false).kind, AssetKind::texture);
    EXPECT_EQ(ed::asset_type("s.slang", false).kind, AssetKind::shader);
    EXPECT_EQ(ed::asset_type("scene.json", false).kind, AssetKind::scene);
    EXPECT_EQ(ed::asset_type("hit.wav", false).kind, AssetKind::audio);
    EXPECT_EQ(ed::asset_type("Inter.ttf", false).kind, AssetKind::font);
    EXPECT_EQ(ed::asset_type("README", false).kind, AssetKind::other);
    for (const char* f : {"x.glb", "x.png", "dir"}) {
        const ed::AssetTypeInfo t = ed::asset_type(f, std::string_view(f) == "dir");
        EXPECT_NE(t.icon, nullptr);
        EXPECT_NE(t.label, nullptr);
    }
}

} // namespace

#endif // ENGINE_EDITOR
