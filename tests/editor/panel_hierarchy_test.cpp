// Hierarchy / asset browser logic (ROADMAP E8.3): type icons, the shared
// create / delete actions behind menus, palette and context menus, rename,
// search and re-parenting (keeping the world pose), and the asset browser's
// file classification and project-confined listing.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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
#include "support/temp_dir.hpp"

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

TEST(EditorHierarchy, RenameAddsANameWhenTheEntityHasNone) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity bare = r.create();
    r.emplace<s::Transform>(bare);
    ASSERT_FALSE(r.all_of<s::Name>(bare));
    EXPECT_EQ(ed::entity_label(r, bare), "entity " + std::to_string(static_cast<std::uint32_t>(bare)));
    ed::commit_rename(r, bare, "Lamp");
    ASSERT_TRUE(r.all_of<s::Name>(bare));
    EXPECT_EQ(r.get<s::Name>(bare).value, "Lamp");
    EXPECT_EQ(ed::entity_label(r, bare), "Lamp");
    ed::commit_rename(r, bare, "Lamp 2");
    EXPECT_EQ(r.get<s::Name>(bare).value, "Lamp 2");
    ed::commit_rename(r, entt::entity{12345}, "x"); // invalid: ignored
}

TEST(EditorHierarchy, SearchMatchesNamesCaseInsensitively) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity a = scene.create_entity("Red Lamp");
    const entt::entity b = scene.create_entity("lamp post");
    (void)scene.create_entity("Crate");
    const auto lamps = ed::filter_entities(r, "LAMP");
    ASSERT_EQ(lamps.size(), 2U);
    EXPECT_TRUE(std::find(lamps.begin(), lamps.end(), a) != lamps.end());
    EXPECT_TRUE(std::find(lamps.begin(), lamps.end(), b) != lamps.end());
    EXPECT_EQ(ed::filter_entities(r, "").size(), 3U);
    EXPECT_TRUE(ed::filter_entities(r, "zebra").empty());
}

TEST(EditorHierarchy, ReparentKeepsTheWorldPose) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity parent = scene.create_entity("parent");
    r.get<s::Transform>(parent).local =
        glm::scale(glm::rotate(glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, -4.0F)),
                               glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F)),
                   glm::vec3(2.0F));
    const entt::entity child = scene.create_entity("child");
    r.get<s::Transform>(child).local = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));
    const glm::mat4 before = sh::current_world(r, child);

    ASSERT_TRUE(sh::reparent_keep_world(scene, child, parent));
    EXPECT_EQ(r.get<s::Parent>(child).entity, parent);
    const glm::mat4 after = sh::current_world(r, child);
    for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_NEAR(after[c][k], before[c][k], 1e-4F) << c << "," << k;
    }
    EXPECT_NEAR(glm::vec3(after[3]).x, 1.0F, 1e-4F);

    // And back to the root.
    ASSERT_TRUE(sh::reparent_keep_world(scene, child, k_none));
    EXPECT_FALSE(r.all_of<s::Parent>(child));
    const glm::mat4 root = r.get<s::Transform>(child).local;
    EXPECT_NEAR(root[3].z, 3.0F, 1e-4F);
    // A cycle is refused and changes nothing.
    ASSERT_TRUE(sh::reparent_keep_world(scene, child, parent));
    const glm::mat4 local = r.get<s::Transform>(child).local;
    EXPECT_FALSE(sh::reparent_keep_world(scene, parent, child));
    EXPECT_EQ(r.get<s::Transform>(child).local, local);
    EXPECT_FALSE(r.all_of<s::Parent>(parent));
}

TEST(EditorAssetBrowser, ListsFoldersFirstAndStaysInsideTheProject) {
    const quat::test::TempDir tmp("assets");
    const std::filesystem::path root = tmp.path() / "project";
    const std::filesystem::path outside = tmp.path() / "outside";
    std::error_code ec;
    std::filesystem::create_directories(root / "assets/models", ec);
    std::filesystem::create_directories(root / "scenes", ec);
    std::filesystem::create_directories(outside, ec);
    for (const auto& f : {root / "assets/b.png", root / "assets/a.glb", root / "assets/.hidden",
                          root / "assets/models/tree.gltf", outside / "secret.json"}) {
        std::FILE* fp = std::fopen(f.c_str(), "wb");
        ASSERT_NE(fp, nullptr) << f;
        std::fclose(fp);
    }
    std::filesystem::create_directory_symlink(outside, root / "assets/escape", ec);
    const bool have_link = !ec;

    const auto top = ed::list_assets(root, "", "");
    ASSERT_EQ(top.size(), 2U) << "the asset roots that exist";
    EXPECT_EQ(top[0].name, "assets");
    EXPECT_EQ(top[1].name, "scenes");

    const auto assets = ed::list_assets(root, "assets", "");
    std::vector<std::string> names;
    for (const auto& e : assets) names.push_back(e.name);
    std::vector<std::string> want = {"models", "a.glb", "b.png"};
    if (have_link) want.insert(want.begin(), "escape");
    EXPECT_EQ(names, want) << "folders first, then files, by name; dot files hidden";

    const auto found = ed::list_assets(root, "", "TREE");
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].name, "assets/models/tree.gltf");

    for (const char* bad : {"..", "../outside", "assets/../../outside", "/etc"}) {
        EXPECT_FALSE(ed::confined_dir(root, bad).has_value()) << bad;
        EXPECT_TRUE(ed::list_assets(root, bad, "").empty()) << bad;
    }
    EXPECT_TRUE(ed::confined_dir(root, "assets/models").has_value());
    if (have_link) {
        EXPECT_FALSE(ed::confined_dir(root, "assets/escape").has_value()) << "symlink out";
        EXPECT_TRUE(ed::list_assets(root, "assets/escape", "").empty());
    }
    EXPECT_TRUE(ed::list_assets(root, "assets/missing", "").empty());
}

} // namespace

#endif // ENGINE_EDITOR
