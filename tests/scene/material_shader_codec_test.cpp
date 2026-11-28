// Material shader selection on primitives (ROADMAP A6): the codec reads and
// writes `shader` / `shader_params`, validates the name, and the resolved
// material carries the shader and stays opaque.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/primitives.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;

class PrimitiveShaderCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(std::string_view name, const json& v) {
        return s::apply_component(r, e, name, v, true);
    }
};

TEST_F(PrimitiveShaderCodec, ReadsAndWritesTheShaderAndItsParameters) {
    ASSERT_OK(apply("primitive", {{"shape", "sphere"},
                                  {"shader", "lava"},
                                  {"shader_params", {1, 0.5, 0.25, 6, 2}}}));
    const s::Primitive& p = r.get<s::Primitive>(e);
    EXPECT_EQ(p.shader, "lava");
    EXPECT_EQ(p.shader_params[0], glm::vec4(1.0F, 0.5F, 0.25F, 6.0F));
    EXPECT_EQ(p.shader_params[1], glm::vec4(2.0F, 0.0F, 0.0F, 0.0F)) << "missing ones are 0";

    const json out = s::encode_components(r, e)["primitive"];
    EXPECT_EQ(out["shader"], "lava");
    EXPECT_EQ(out["shader_params"], json({1.0, 0.5, 0.25, 6.0, 2.0, 0.0, 0.0, 0.0}));

    // Merge: other edits keep the shader; "standard" (or "") selects the built-in.
    ASSERT_OK(apply("primitive", {{"roughness", 0.2}}));
    EXPECT_EQ(r.get<s::Primitive>(e).shader, "lava");
    ASSERT_OK(apply("primitive", {{"shader", "standard"}}));
    EXPECT_EQ(r.get<s::Primitive>(e).shader, "");
    EXPECT_EQ(s::encode_components(r, e)["primitive"]["shader"], "standard");
}

TEST_F(PrimitiveShaderCodec, RejectsBadNamesAndParameters) {
    ASSERT_OK(apply("primitive", {{"shader", "lava"}}));
    for (const json& bad : {json("../lava"), json("Lava"), json("a b"), json(7),
                            json(std::string(49, 'a'))}) {
        const auto r1 = apply("primitive", {{"shader", bad}});
        ASSERT_FALSE(r1) << bad.dump();
        EXPECT_NE(r1.error().message.find("shader"), std::string::npos) << r1.error().message;
    }
    EXPECT_FALSE(apply("primitive", {{"shader_params", {1, 2, 3, 4, 5, 6, 7, 8, 9}}}));
    EXPECT_FALSE(apply("primitive", {{"shader_params", {"x"}}}));
    EXPECT_FALSE(apply("primitive", {{"shader_params", 3}}));
    EXPECT_EQ(r.get<s::Primitive>(e).shader, "lava") << "failed edits change nothing";
}

TEST_F(PrimitiveShaderCodec, InstancedMeshPrimitivesTakeAShaderToo) {
    ASSERT_OK(apply("instanced_mesh", {{"shape", "cube"}, {"shader", "lava"}}));
    EXPECT_EQ(r.get<s::InstancedMesh>(e).primitive.shader, "lava");
    EXPECT_EQ(s::encode_components(r, e)["instanced_mesh"]["shader"], "lava");
}

TEST(PrimitiveShaderMaterial, ShaderSelectionChangesTheMaterialAndStaysOpaque) {
    s::Primitive a;
    s::Primitive b = a;
    b.shader = "lava";
    EXPECT_NE(s::primitive_hash(a), s::primitive_hash(b));
    s::Primitive c = b;
    c.shader_params[1].y = 3.0F;
    EXPECT_NE(s::primitive_hash(b), s::primitive_hash(c));

    c.color.a = 0.25F;
    const engine::asset::PbrMaterialParams params = s::primitive_material_params(c);
    EXPECT_EQ(params.flags & engine::asset::material_blend, 0U)
        << "material shaders render in the deferred pass";
    EXPECT_FLOAT_EQ(params.base_color_factor.a, 0.25F) << "the shader still sees the alpha";
    EXPECT_EQ(params.shader_params[1], glm::vec4(0.0F, 3.0F, 0.0F, 0.0F));
}

} // namespace
