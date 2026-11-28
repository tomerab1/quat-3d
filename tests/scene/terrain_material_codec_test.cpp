// terrain.material codec (R26.1): presets by name, overrides, layer merges,
// round trips and precise errors for remote clients.

#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;
namespace t = engine::terrain;

class TerrainMaterialCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(const json& v, bool strict = true) {
        return s::apply_component(r, e, "terrain", v, strict);
    }
    [[nodiscard]] const t::TerrainMaterial& material() const {
        return r.get<s::Terrain>(e).material;
    }
    [[nodiscard]] json encoded() const { return s::encode_components(r, e, {})["terrain"]["material"]; }
};

TEST_F(TerrainMaterialCodec, NewTerrainUsesTheDefaultPreset) {
    ASSERT_OK(apply(json::object()));
    EXPECT_EQ(material().preset, "temperate_meadow");
    EXPECT_EQ(material().layers.size(), t::default_terrain_material().layers.size());
    EXPECT_EQ(encoded()["layers"][0]["texture"], "grass");
}

TEST_F(TerrainMaterialCodec, PresetByNameOrInsideAnObject) {
    ASSERT_OK(apply({{"material", "alpine"}}));
    EXPECT_EQ(material().preset, "alpine");
    ASSERT_OK(apply({{"material", {{"preset", "desert"}, {"macro_strength", 0.5}}}}));
    EXPECT_EQ(material().preset, "desert");
    EXPECT_FLOAT_EQ(material().macro_strength, 0.5F);
    EXPECT_EQ(material().layers[0].texture_set, "sand");
}

TEST_F(TerrainMaterialCodec, LayersMergeByIndexAndReplaceTheStack) {
    ASSERT_OK(apply({{"material", "temperate_meadow"}}));
    const std::string first_name = material().layers[0].name;
    ASSERT_OK(apply({{"material",
                      {{"layers",
                        {json::object(),
                         {{"texture", "rock"}, {"slope", {30, 90}}, {"paint", "g"}},
                         {{"texture", "sand"}, {"altitude", {-5, 2}}, {"altitude_ref", "world"}}}}}}}));
    ASSERT_EQ(material().layers.size(), 3U);
    EXPECT_EQ(material().layers[0].name, first_name); // empty entry keeps layer 0
    EXPECT_EQ(material().layers[1].texture_set, "rock");
    EXPECT_FLOAT_EQ(material().layers[1].rule.slope_deg.x, 30.0F);
    EXPECT_EQ(material().layers[1].rule.paint, t::PaintChannel::g);
    EXPECT_EQ(material().layers[2].texture_set, "sand");
    EXPECT_FLOAT_EQ(material().layers[2].rule.altitude_m.y, 2.0F);
}

TEST_F(TerrainMaterialCodec, RoundTrips) {
    ASSERT_OK(apply({{"material", "forest_floor"}}));
    const json first = encoded();
    entt::registry r2;
    const entt::entity e2 = r2.create();
    ASSERT_OK(s::apply_component(r2, e2, "terrain", {{"material", first}}, true));
    EXPECT_EQ(s::encode_components(r2, e2, {})["terrain"]["material"], first);
}

TEST_F(TerrainMaterialCodec, MaterialEditsDoNotRegenerate) {
    ASSERT_OK(apply(json::object()));
    r.get<s::Terrain>(e).regenerate = false;
    ASSERT_OK(apply({{"material", "alpine"}}));
    EXPECT_FALSE(r.get<s::Terrain>(e).regenerate);
}

TEST_F(TerrainMaterialCodec, RejectsBadInputPrecisely) {
    ASSERT_OK(apply(json::object()));
    auto bad = apply({{"material", "lunar"}});
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("temperate_meadow"), std::string::npos) << bad.error().message;

    bad = apply({{"material", {{"layers", {{{"texture", "lava"}}}}}}});
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("layers[0].texture"), std::string::npos) << bad.error().message;

    bad = apply({{"material", {{"layers", {{{"slope", {60, 10}}}}}}}});
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("slope"), std::string::npos) << bad.error().message;

    bad = apply({{"material", {{"layers", json::array()}}}});
    EXPECT_FALSE(bad);
    json nine = json::array();
    for (int i = 0; i < 9; ++i) nine.push_back(json::object());
    EXPECT_FALSE(apply({{"material", {{"layers", nine}}}}));
    EXPECT_FALSE(apply({{"material", {{"blend_depth", 5}}}}));
    EXPECT_FALSE(apply({{"material", {{"colour", 1}}}})); // unknown field (strict)
    // A failed edit leaves the material as it was.
    EXPECT_EQ(material().preset, "temperate_meadow");
}

} // namespace
