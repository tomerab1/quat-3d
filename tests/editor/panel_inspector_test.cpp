// Inspector section actions (ROADMAP E8.3): ⋮ > Reset / Copy / Paste /
// Remove and the Add Component list, run without a frame. Reset works in
// place (it never removes the component, so nothing that depends on it goes
// with it); Remove goes through the scene codec like the remote API.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/panels/inspector_parts.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace p = engine::editor::panels;
namespace s = engine::scene;
using nlohmann::json;

json encoded(const entt::registry& r, entt::entity e, std::string_view codec) {
    const std::string only[] = {std::string(codec)};
    s::EncodeOptions opts;
    opts.only = only;
    const json j = s::encode_components(r, e, opts);
    const auto it = j.find(only[0]);
    return it != j.end() ? *it : json();
}

bool has(const std::vector<std::string_view>& v, std::string_view x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

TEST(EditorInspector, ResetRestoresEveryAddableCodecInPlace) {
    for (const s::ComponentInfo& info : s::component_infos()) {
        if (!info.addable || !info.writable) continue;
        SCOPED_TRACE(std::string(info.name));
        s::Scene scene;
        entt::registry& r = scene.registry();
        const entt::entity e = scene.create_entity("e");
        ASSERT_OK(s::apply_component(r, e, info.name, json::object(), false));
        EXPECT_TRUE(p::can_reset(info.name));
        // An empty material override is no override (covered below).
        if (!has(s::component_names(r, e), info.name)) continue;
        const json defaults = encoded(r, e, info.name);
        ASSERT_OK(p::reset_component(r, e, info.name));
        EXPECT_TRUE(has(s::component_names(r, e), info.name))
            << "reset must not remove the component";
        EXPECT_EQ(encoded(r, e, info.name), defaults);
    }
}

TEST(EditorInspector, ResetUndoesEditsAndClearsOverrides) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity e = scene.create_entity("lamp");
    ASSERT_OK(s::apply_component(r, e, "point_light", json::object(), false));
    const json defaults = encoded(r, e, "point_light");
    ASSERT_OK(s::apply_component(r, e, "point_light", json{{"intensity", 42.0}, {"radius", 3.0}}, true));
    ASSERT_NE(encoded(r, e, "point_light"), defaults);
    ASSERT_OK(p::reset_component(r, e, "point_light"));
    EXPECT_EQ(encoded(r, e, "point_light"), defaults);

    // Optional overrides the defaults leave out are cleared.
    ASSERT_OK(s::apply_component(r, e, "material", json{{"roughness", 0.9}}, true));
    ASSERT_TRUE(has(s::component_names(r, e), "material"));
    ASSERT_OK(p::reset_component(r, e, "material"));
    EXPECT_FALSE(has(s::component_names(r, e), "material"));

    // The transform resets to identity.
    r.get<s::Transform>(e).local[3] = glm::vec4(1.0F, 2.0F, 3.0F, 1.0F);
    ASSERT_OK(p::reset_component(r, e, "transform"));
    EXPECT_EQ(r.get<s::Transform>(e).local, glm::mat4(1.0F));
}

TEST(EditorInspector, ResetKeepsWhatDependsOnTheComponent) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    // A Primitive's generated MeshRenderer stays.
    const entt::entity cube = scene.create_entity("cube");
    ASSERT_OK(s::apply_component(r, cube, "primitive", json{{"shape", "sphere"}}, true));
    r.emplace_or_replace<s::MeshRenderer>(cube);
    ASSERT_OK(p::reset_component(r, cube, "primitive"));
    EXPECT_TRUE(r.all_of<s::Primitive>(cube));
    EXPECT_TRUE(r.all_of<s::MeshRenderer>(cube));

    // An instanced mesh keeps its parts, its scatter and its placements.
    const entt::entity trees = scene.create_entity("trees");
    ASSERT_OK(s::apply_component(r, trees, "instanced_mesh", json::object(), false));
    ASSERT_OK(s::apply_component(r, trees, "scatter", json::object(), false));
    r.emplace_or_replace<s::InstancedMeshParts>(trees);
    r.get<s::InstancedMesh>(trees).instances.resize(5);
    ASSERT_OK(p::reset_component(r, trees, "instanced_mesh"));
    EXPECT_TRUE(r.all_of<s::InstancedMesh>(trees));
    EXPECT_TRUE(r.all_of<s::Scatter>(trees));
    EXPECT_TRUE(r.all_of<s::InstancedMeshParts>(trees));
    EXPECT_EQ(r.get<s::InstancedMesh>(trees).instances.size(), 5U);
}

TEST(EditorInspector, ResetIsRefusedForAssetDrivenComponents) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity e = scene.create_entity("rig");
    r.emplace<s::Animator>(e);
    EXPECT_FALSE(p::can_reset("animator"));
    EXPECT_FALSE(p::can_reset("mesh"));
    EXPECT_FALSE(p::can_reset("no_such_component"));
    EXPECT_ERR(p::reset_component(r, e, "animator"));
    EXPECT_TRUE(r.all_of<s::Animator>(e)) << "a refused reset leaves the Animator in place";
}

TEST(EditorInspector, CopyPasteMovesValuesBetweenEntities) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity a = scene.create_entity("a");
    const entt::entity b = scene.create_entity("b");
    ASSERT_OK(s::apply_component(r, a, "point_light", json{{"intensity", 7.0}}, false));
    ASSERT_OK(s::apply_component(r, b, "point_light", json::object(), false));

    p::InspectorState st;
    EXPECT_FALSE(p::copy_component(r, a, "camera").has_value()) << "a has no camera";
    const auto text = p::copy_component(r, a, "point_light");
    ASSERT_TRUE(text.has_value());
    st.clipboard_type = "point_light";
    st.clipboard_json = *text;
    ASSERT_OK(p::paste_component(r, b, st, "point_light"));
    EXPECT_FLOAT_EQ(r.get<s::PointLight>(b).intensity, 7.0F);

    EXPECT_ERR(p::paste_component(r, b, st, "camera")) << "type mismatch";
    st.clipboard_json = "{not json";
    EXPECT_ERR(p::paste_component(r, b, st, "point_light"));
}

TEST(EditorInspector, RemoveGoesThroughTheCodec) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity cube = scene.create_entity("cube");
    ASSERT_OK(s::apply_component(r, cube, "primitive", json::object(), false));
    r.emplace_or_replace<s::MeshRenderer>(cube);
    EXPECT_TRUE(p::remove_section(r, cube, "primitive"));
    EXPECT_FALSE(r.all_of<s::Primitive>(cube));
    EXPECT_FALSE(r.all_of<s::MeshRenderer>(cube)) << "the Primitive's MeshRenderer goes with it";

    const entt::entity trees = scene.create_entity("trees");
    ASSERT_OK(s::apply_component(r, trees, "instanced_mesh", json::object(), false));
    ASSERT_OK(s::apply_component(r, trees, "scatter", json::object(), false));
    r.emplace_or_replace<s::InstancedMeshParts>(trees);
    EXPECT_TRUE(p::remove_section(r, trees, "instanced_mesh"));
    EXPECT_FALSE((r.any_of<s::InstancedMesh, s::InstancedMeshParts, s::Scatter>(trees)));

    EXPECT_FALSE(p::remove_section(r, cube, nullptr)) << "no codec: the caller removes it";
}

TEST(EditorInspector, AddComponentListsWhatTheEntityLacks) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity e = scene.create_entity("e");
    const auto all = p::addable_components(r, e, "");
    EXPECT_TRUE(has(all, "point_light"));
    EXPECT_TRUE(has(all, "camera"));
    EXPECT_FALSE(has(all, "transform")) << "not addable";
    EXPECT_FALSE(has(all, "animator")) << "comes with a model";
    EXPECT_FALSE(has(all, "foot_ik")) << "only for skinned meshes";

    ASSERT_OK(s::apply_component(r, e, "point_light", json::object(), false));
    EXPECT_FALSE(has(p::addable_components(r, e, ""), "point_light")) << "already present";

    const auto lights = p::addable_components(r, e, "LIGHT");
    EXPECT_TRUE(has(lights, "dir_light")) << "case-insensitive title match";
    EXPECT_FALSE(has(lights, "camera"));
    EXPECT_TRUE(has(p::addable_components(r, e, "sun"), "dir_light")) << "matches the summary";
    EXPECT_TRUE(p::addable_components(r, e, "zzzz-nothing").empty());

    r.emplace<s::SkinnedMesh>(e);
    EXPECT_TRUE(has(p::addable_components(r, e, ""), "foot_ik"));
}

} // namespace

#endif // ENGINE_EDITOR
