// Material tuning (A8): the field table, the `material` override codec, where
// edits land (Primitive vs. MaterialOverride) and which material is drawn.
// Device-free: materials come from AssetManager loaders without GPU buffers.

#include <set>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
namespace asset = engine::asset;
using nlohmann::json;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const s::MaterialFieldInfo& field(std::string_view name) {
    const s::MaterialFieldInfo* f = s::find_material_field(name);
    EXPECT_NE(f, nullptr) << name;
    return *f;
}

asset::AssetHandle<asset::MaterialAsset> cpu_material(asset::AssetManager& assets,
                                                      const std::string& key,
                                                      const asset::PbrMaterialParams& params) {
    return assets.load<asset::MaterialAsset>(key, [&](const std::filesystem::path&) {
        asset::MaterialAsset m;
        m.params = params;
        return std::expected<asset::MaterialAsset, engine::core::Error>(std::move(m));
    });
}

TEST(MaterialOverride, FieldTableIsConsistent) {
    std::set<std::string> names;
    std::uint32_t bits = 0;
    for (const s::MaterialFieldInfo& f : s::material_fields()) {
        EXPECT_TRUE(names.insert(std::string(f.name)).second) << f.name;
        EXPECT_EQ(bits & f.bit, 0U) << f.name << " shares a bit";
        bits |= f.bit;
        EXPECT_LT(f.min, f.max) << f.name;
        EXPECT_FALSE(f.hint.empty()) << f.name;
        // Each field reads back what it stores (clamped).
        asset::PbrMaterialParams p;
        const glm::vec4 v(f.max, f.min, f.max, f.min);
        s::set_material_field_value(p, f, v);
        const glm::vec4 back = s::material_field_value(p, f);
        EXPECT_FLOAT_EQ(back.x, f.kind == s::MaterialFieldKind::color4 ? 1.0F : f.max) << f.name;
    }
    EXPECT_EQ(s::material_fields_schema().size(), s::material_fields().size());
}

TEST(MaterialOverride, TransmissionTogglesTheGlassFlag) {
    asset::PbrMaterialParams p;
    s::set_material_field_value(p, field("transmission"), glm::vec4(0.8F));
    EXPECT_NE(p.flags & asset::material_transmission, 0U);
    s::set_material_field_value(p, field("transmission"), glm::vec4(0.0F));
    EXPECT_EQ(p.flags & asset::material_transmission, 0U);
}

TEST(MaterialOverride, CodecMergesClearsAndRejects) {
    s::MaterialOverride o;
    ASSERT_OK(s::decode_material_override(
        json{{"roughness", 0.2}, {"base_color", "#ff0000"}, {"emissive", {5, 0, 0}}}, o, true));
    EXPECT_EQ(o.mask, field("roughness").bit | field("base_color").bit | field("emissive").bit);
    EXPECT_FLOAT_EQ(o.values.roughness_factor, 0.2F);
    EXPECT_FLOAT_EQ(o.values.base_color_factor.r, 1.0F);
    EXPECT_FLOAT_EQ(o.values.emissive_factor.x, 5.0F);

    // null drops one override, absent fields stay.
    ASSERT_OK(s::decode_material_override(json{{"roughness", nullptr}}, o, true));
    EXPECT_EQ(o.mask, field("base_color").bit | field("emissive").bit);
    const json enc = s::encode_material_override(o);
    EXPECT_EQ(enc.size(), 2U);
    EXPECT_EQ(enc["emissive"], json({5.0, 0.0, 0.0}));

    // Errors name the field and change nothing.
    const std::uint32_t before = o.mask;
    auto e = s::decode_material_override(json{{"metallic", 0.5}, {"roughness", 2}}, o, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "material.roughness: expected a number in [0, 1]"))
        << e.error().message;
    EXPECT_EQ(o.mask, before);
    e = s::decode_material_override(json{{"shininess", 1}}, o, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "material.shininess: unknown field")) << e.error().message;
    EXPECT_FALSE(s::decode_material_override(json{{"attenuation_color", {2, 0, 0}}}, o, true));
    EXPECT_FALSE(s::decode_material_override(json{{"emissive", "#ff000080"}}, o, true));
    // Lenient (scene files): unknown fields are ignored.
    EXPECT_OK(s::decode_material_override(json{{"shininess", 1}}, o, false));
}

TEST(MaterialOverride, PrimitiveFieldsEditThePrimitive) {
    entt::registry r;
    const entt::entity e = r.create();
    auto& prim = r.emplace<s::Primitive>(e);
    prim.roughness = 0.5F;

    s::set_entity_material_field(r, e, field("roughness"), glm::vec4(0.1F));
    EXPECT_FLOAT_EQ(r.get<s::Primitive>(e).roughness, 0.1F);
    EXPECT_FALSE(r.all_of<s::MaterialOverride>(e));

    // Factors a primitive does not own become an override on top of it.
    s::set_entity_material_field(r, e, field("clearcoat"), glm::vec4(1.0F));
    ASSERT_TRUE(r.all_of<s::MaterialOverride>(e));
    const auto p = s::effective_material_params(r, e);
    ASSERT_TRUE(p.has_value());
    EXPECT_FLOAT_EQ(p->roughness_factor, 0.1F);
    EXPECT_FLOAT_EQ(p->clearcoat_factor, 1.0F);

    // An override of a primitive field (e.g. from scene.update) is replaced by
    // a direct edit, so the primitive's own value shows again.
    ASSERT_OK(s::apply_component(r, e, "material", json{{"metallic", 1.0}}, true));
    EXPECT_FLOAT_EQ(s::effective_material_params(r, e)->metallic_factor, 1.0F);
    s::set_entity_material_field(r, e, field("metallic"), glm::vec4(0.25F));
    EXPECT_FLOAT_EQ(r.get<s::Primitive>(e).metallic, 0.25F);
    EXPECT_FLOAT_EQ(s::effective_material_params(r, e)->metallic_factor, 0.25F);

    s::clear_material_override_field(r, e, field("clearcoat"));
    EXPECT_FALSE(r.all_of<s::MaterialOverride>(e)); // empty overrides go away
}

TEST(MaterialOverride, MeshMaterialsAreOverriddenPerEntity) {
    asset::AssetManager assets;
    asset::PbrMaterialParams base;
    base.roughness_factor = 0.8F;
    base.flags = asset::material_has_base_color;
    const auto handle = cpu_material(assets, "test/shared", base);
    entt::registry r;
    const entt::entity a = r.create();
    const entt::entity b = r.create();
    r.emplace<s::MeshRenderer>(a, asset::AssetHandle<asset::MeshAsset>{}, handle);
    r.emplace<s::MeshRenderer>(b, asset::AssetHandle<asset::MeshAsset>{}, handle);
    EXPECT_FALSE(s::base_material_params(r, r.create()).has_value()); // no material

    s::set_entity_material_field(r, a, field("roughness"), glm::vec4(0.2F));
    EXPECT_FLOAT_EQ(s::effective_material_params(r, a)->roughness_factor, 0.2F);
    EXPECT_FLOAT_EQ(s::effective_material_params(r, b)->roughness_factor, 0.8F);
    EXPECT_FLOAT_EQ(handle->params.roughness_factor, 0.8F); // shared asset untouched

    // Until resolved the base material is drawn; the override needs a resolve.
    const auto& mr = r.get<s::MeshRenderer>(a);
    EXPECT_EQ(s::drawn_material(r, a, mr), &*handle);
    EXPECT_TRUE(s::material_overrides_need_resolve(r));

    // Simulate a resolve (the GPU suite runs the real one).
    auto& o = r.get<s::MaterialOverride>(a);
    const auto derived = cpu_material(assets, "test/derived", s::apply_material_override(base, o));
    o.derived = derived;
    o.base_id = handle.id();
    EXPECT_EQ(s::drawn_material(r, a, mr), &*derived);
    EXPECT_EQ(s::drawn_material(r, b, r.get<s::MeshRenderer>(b)), &*handle);

    // A different base (e.g. a primitive rebuilt its material) is drawn as-is
    // until the override is resolved again.
    const auto other = cpu_material(assets, "test/other", base);
    r.get<s::MeshRenderer>(a).material = other;
    EXPECT_EQ(s::drawn_material(r, a, r.get<s::MeshRenderer>(a)), &*other);

    // The codec carries the overrides; removing the component reverts.
    const json comps = s::encode_components(r, a, {});
    ASSERT_TRUE(comps.contains("material"));
    EXPECT_FLOAT_EQ(comps["material"]["roughness"].get<float>(), 0.2F);
    EXPECT_TRUE(s::remove_component(r, a, "material"));
    EXPECT_FLOAT_EQ(s::effective_material_params(r, a)->roughness_factor, 0.8F);
    EXPECT_EQ(s::drawn_material(r, a, r.get<s::MeshRenderer>(a)), &*other);
}

} // namespace
