// Codec entries of the R9.2 components (instanced_mesh, scatter): what scene
// files and the remote API read and write.

#include <algorithm>
#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using json = nlohmann::json;

class InstancedCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(std::string_view name, const json& v, bool strict = true) {
        return s::apply_component(r, e, name, v, strict);
    }
};

TEST_F(InstancedCodec, AuthoredPlacementsRoundTrip) {
    ASSERT_OK(apply("instanced_mesh",
                    {{"shape", "sphere"},
                     {"instances",
                      {{{"position", {1, 2, 3}}, {"rotation", {10, 45, -20}}, {"scale", 2}},
                       {{"position", {-4, 0, 0}}, {"scale", {1, 2, 3}}},
                       {5, 6, 7}}}}));
    const auto& m = r.get<s::InstancedMesh>(e);
    EXPECT_EQ(m.primitive.shape, s::PrimitiveShape::sphere);
    ASSERT_EQ(m.instances.size(), 3U);
    EXPECT_EQ(m.instances[1].scale, glm::vec3(1.0F, 2.0F, 3.0F));
    EXPECT_EQ(m.instances[2].position, glm::vec3(5.0F, 6.0F, 7.0F)); // bare position
    EXPECT_EQ(m.instances[2].scale, glm::vec3(1.0F));

    const json encoded = s::encode_components(r, e);
    ASSERT_TRUE(encoded.contains("instanced_mesh"));
    EXPECT_EQ(encoded["instanced_mesh"]["instance_count"], 3);
    const entt::entity copy = r.create();
    ASSERT_OK(s::apply_component(r, copy, "instanced_mesh", encoded["instanced_mesh"], true));
    const auto& c = r.get<s::InstancedMesh>(copy);
    ASSERT_EQ(c.instances.size(), 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(c.instances[i].position, m.instances[i].position) << i;
        EXPECT_EQ(c.instances[i].scale, m.instances[i].scale) << i;
        // Same rotation (q and -q are the same rotation).
        EXPECT_NEAR(std::abs(glm::dot(c.instances[i].rotation, m.instances[i].rotation)), 1.0F,
                    1e-5F)
            << i;
    }
    // The Euler convention matches `transform`: yaw 45 about +Y.
    const glm::quat q = glm::angleAxis(glm::radians(45.0F), glm::vec3(0, 1, 0)) *
                        glm::angleAxis(glm::radians(10.0F), glm::vec3(1, 0, 0)) *
                        glm::angleAxis(glm::radians(-20.0F), glm::vec3(0, 0, 1));
    EXPECT_NEAR(std::abs(glm::dot(q, m.instances[0].rotation)), 1.0F, 1e-5F);
}

TEST_F(InstancedCodec, MergeKeepsPlacementsWhenNotMentioned) {
    ASSERT_OK(apply("instanced_mesh", {{"instances", {{0, 0, 0}, {1, 0, 0}}}}));
    ASSERT_OK(apply("instanced_mesh", {{"color", "#ff0000"}}));
    EXPECT_EQ(r.get<s::InstancedMesh>(e).instances.size(), 2U);
    ASSERT_OK(apply("instanced_mesh", {{"instances", json::array()}}));
    EXPECT_TRUE(r.get<s::InstancedMesh>(e).instances.empty());
}

TEST_F(InstancedCodec, RejectsBadInput) {
    EXPECT_ERR(apply("instanced_mesh", {{"gltf", "/etc/passwd"}}));
    EXPECT_ERR(apply("instanced_mesh", {{"gltf", "../outside/tree.glb"}}));
    EXPECT_ERR(apply("instanced_mesh", {{"gltf", "assets/../../tree.glb"}}));
    EXPECT_ERR(apply("instanced_mesh", {{"instances", {{{"position", {1, 2}}}}}}));
    EXPECT_ERR(apply("instanced_mesh", {{"instances", {{{"scale", 0}}}}}));
    EXPECT_ERR(apply("instanced_mesh", {{"instances", {{{"positon", {1, 2, 3}}}}}}));
    EXPECT_ERR(apply("instanced_mesh", {{"instances", "many"}}));
    EXPECT_ERR(apply("instanced_mesh", {{"shape", "tree"}}));
    EXPECT_ERR(apply("instanced_mesh", {{"metallic", 2}}));
    EXPECT_FALSE(r.all_of<s::InstancedMesh>(e)); // nothing was applied
    // Lenient (scene files): unknown fields are ignored, bad values are not.
    EXPECT_OK(apply("instanced_mesh", {{"future_field", 1}}, false));
    EXPECT_ERR(apply("instanced_mesh", {{"gltf", 5}}, false));

    EXPECT_ERR(apply("scatter", {{"density", -1}}));
    EXPECT_ERR(apply("scatter", {{"scale", {2, 1}}}));
    EXPECT_ERR(apply("scatter", {{"slope", {0, 120}}}));
    EXPECT_ERR(apply("scatter", {{"size", {10}}}));
    EXPECT_ERR(apply("scatter", {{"area", "hexagon"}}));
    EXPECT_ERR(apply("scatter", {{"max_instances", 1000000}}));
    EXPECT_ERR(apply("scatter", {{"denisty", 1}}));
    EXPECT_FALSE(r.all_of<s::Scatter>(e));
}

TEST_F(InstancedCodec, ScatteredPlacementsAreDerived) {
    ASSERT_OK(apply("scatter", {{"seed", 5}, {"density", 0.1}}));
    auto& mesh = r.emplace<s::InstancedMesh>(e);
    mesh.instances.resize(40); // as if scatter_system ran

    const json encoded = s::encode_components(r, e);
    EXPECT_FALSE(encoded["instanced_mesh"].contains("instances"));
    EXPECT_EQ(encoded["instanced_mesh"]["instance_count"], 40);
    EXPECT_EQ(encoded["scatter"]["seed"], 5);

    // Hand placements would be overwritten by the next regeneration: refused.
    EXPECT_ERR(apply("instanced_mesh", {{"instances", {{0, 0, 0}}}}));
    EXPECT_OK(apply("instanced_mesh", {{"shape", "cone"}, {"instance_count", 40}}));
    EXPECT_EQ(r.get<s::InstancedMesh>(e).instances.size(), 40U);

    const auto names = s::component_names(r, e);
    EXPECT_NE(std::find(names.begin(), names.end(), "instanced_mesh"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "scatter"), names.end());
    ASSERT_NE(s::find_component_info("scatter"), nullptr);
    EXPECT_TRUE(s::find_component_info("instanced_mesh")->addable);

    // Removing the scatter bakes the placements; removing the mesh drops both.
    EXPECT_TRUE(s::remove_component(r, e, "scatter"));
    EXPECT_EQ(r.get<s::InstancedMesh>(e).instances.size(), 40U);
    EXPECT_TRUE(s::encode_components(r, e)["instanced_mesh"].contains("instances"));
    ASSERT_OK(apply("scatter", json::object()));
    r.emplace<s::InstancedMeshParts>(e);
    EXPECT_TRUE(s::remove_component(r, e, "instanced_mesh"));
    const bool left = r.any_of<s::InstancedMesh, s::InstancedMeshParts, s::Scatter>(e);
    EXPECT_FALSE(left);
}

TEST_F(InstancedCodec, ScatterMergeKeepsOtherFields) {
    ASSERT_OK(apply("scatter", {{"seed", 3}, {"slope", {10, 30}}, {"area", "circle"}}));
    ASSERT_OK(apply("scatter", {{"radius", 50}}));
    const auto& sc = r.get<s::Scatter>(e);
    EXPECT_EQ(sc.seed, 3U);
    EXPECT_EQ(sc.slope_deg, glm::vec2(10.0F, 30.0F));
    EXPECT_EQ(sc.area, s::ScatterArea::circle);
    EXPECT_FLOAT_EQ(sc.radius, 50.0F);
}

} // namespace
