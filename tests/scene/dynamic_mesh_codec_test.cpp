#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/dynamic_mesh.hpp"
#include "support/expect_ok.hpp"

namespace s = engine::scene;
using nlohmann::json;

TEST(DynamicMeshCodec, RoundTrip) {
    entt::registry r;
    const entt::entity e = r.create();
    const json value = {{"mesh", "house_roof"},
                        {"generator", {{"type", "roof"}, {"polygon", {{0, 0}, {4, 0}, {4, 3}, {0, 3}}}}},
                        {"material", {{"color", "#a5452f"}, {"roughness", 0.6}}},
                        {"collider", "mesh"}};
    ASSERT_OK(s::apply_component(r, e, "dynamic_mesh", value, true));
    const auto& d = r.get<s::DynamicMesh>(e);
    EXPECT_EQ(d.mesh, "house_roof");
    EXPECT_EQ(d.collider, s::DynamicCollider::mesh);
    EXPECT_FLOAT_EQ(d.material.roughness, 0.6F);
    EXPECT_EQ(d.generator["type"], "roof");

    const json out = s::encode_components(r, e)["dynamic_mesh"];
    EXPECT_EQ(out["mesh"], "house_roof");
    EXPECT_EQ(out["collider"], "mesh");
    EXPECT_EQ(out["generator"], value["generator"]);
    EXPECT_FALSE(out["material"].contains("shape"));
    // Decoding the encoding gives the same component.
    const entt::entity copy = r.create();
    ASSERT_OK(s::apply_component(r, copy, "dynamic_mesh", out, true));
    EXPECT_EQ(s::encode_components(r, copy)["dynamic_mesh"], out);
}

TEST(DynamicMeshCodec, RejectsBadFields) {
    entt::registry r;
    const entt::entity e = r.create();
    EXPECT_ERR(s::apply_component(r, e, "dynamic_mesh", json{{"mesh", "has space"}}, true));
    EXPECT_ERR(s::apply_component(r, e, "dynamic_mesh", json{{"mesh", "a"}, {"generator", 3}}, true));
    EXPECT_ERR(s::apply_component(r, e, "dynamic_mesh", json{{"mesh", "a"}, {"collider", "box"}}, true));
    EXPECT_ERR(s::apply_component(r, e, "dynamic_mesh",
                                  json{{"mesh", "a"}, {"material", {{"roughness", 2.0}}}}, true));
    EXPECT_ERR(s::apply_component(r, e, "dynamic_mesh",
                                  json{{"mesh", "a"}, {"material", {{"sheen", 1}}}}, true));
    // A primitive and a dynamic mesh would both build the MeshRenderer.
    const entt::entity p = r.create();
    ASSERT_OK(s::apply_component(r, p, "primitive", json{{"shape", "cube"}}, true));
    EXPECT_ERR(s::apply_component(r, p, "dynamic_mesh", json{{"mesh", "a"}}, true));
    const entt::entity d = r.create();
    ASSERT_OK(s::apply_component(r, d, "dynamic_mesh", json{{"mesh", "a"}}, true));
    EXPECT_ERR(s::apply_component(r, d, "primitive", json{{"shape", "cube"}}, true));
    ASSERT_NE(s::find_component_info("dynamic_mesh"), nullptr);
}
