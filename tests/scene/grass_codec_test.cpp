// Grass component codec (R9.3): scene files + remote API.

#include <algorithm>
#include <cmath>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;

class GrassCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(const json& v, bool strict = true) {
        return s::apply_component(r, e, "grass", v, strict);
    }
    [[nodiscard]] const engine::renderer::GrassSettings& grass() const {
        return r.get<s::Grass>(e).settings;
    }
};

TEST_F(GrassCodec, RoundTripsEveryField) {
    const json authored = {{"seed", 99},
                           {"density", 55.5},
                           {"radius", 30},
                           {"blade_height", {0.2, 1.1}},
                           {"blade_width", {0.02, 0.07}},
                           {"base_color", {0.05, 0.1, 0.02}},
                           {"tip_color", {0.4, 0.45, 0.1}},
                           {"wind_strength", 0.8},
                           {"wind_direction", {0, 1}},
                           {"wind_frequency", 1.5},
                           {"slope", {2, 35}},
                           {"height", {-5, 120}},
                           {"terrain_only", false},
                           {"ground_y", 1.25},
                           {"lod_start", 0.4},
                           {"far_density", 0.1}};
    ASSERT_OK(apply(authored));
    EXPECT_EQ(grass().seed, 99U);
    EXPECT_FLOAT_EQ(grass().density, 55.5F);
    EXPECT_EQ(grass().height_range, glm::vec2(0.2F, 1.1F));
    EXPECT_EQ(grass().wind_direction, glm::vec2(0.0F, 1.0F));
    EXPECT_FALSE(grass().terrain_only);
    EXPECT_FLOAT_EQ(grass().ground_y, 1.25F);

    const json encoded = s::encode_components(r, e).at("grass");
    EXPECT_EQ(encoded.size(), authored.size());
    const entt::entity copy = r.create();
    ASSERT_OK(s::apply_component(r, copy, "grass", encoded, true));
    EXPECT_EQ(s::encode_components(r, copy).at("grass"), encoded);
    const auto names = s::component_names(r, e);
    EXPECT_NE(std::ranges::find(names, std::string_view("grass")), names.end());
}

TEST_F(GrassCodec, ColoursAcceptSrgbHexAndStoreLinear) {
    ASSERT_OK(apply({{"base_color", "#808080"}, {"tip_color", "#ffffff"}}));
    EXPECT_NEAR(grass().base_color.r, 0.2158F, 1e-3F); // sRGB 128 -> linear
    EXPECT_NEAR(grass().tip_color.g, 1.0F, 1e-6F);
}

TEST_F(GrassCodec, NormalisesTheWindAndMergesFields) {
    ASSERT_OK(apply({{"wind_direction", {3, 4}}}));
    EXPECT_NEAR(grass().wind_direction.x, 0.6F, 1e-6F);
    EXPECT_NEAR(grass().wind_direction.y, 0.8F, 1e-6F);
    ASSERT_OK(apply({{"density", 12}}));
    EXPECT_NEAR(grass().wind_direction.y, 0.8F, 1e-6F); // kept
    EXPECT_FLOAT_EQ(grass().density, 12.0F);
}

TEST_F(GrassCodec, RejectsMalformedValues) {
    const json cases[] = {
        {{"density", -1}},
        {{"density", 5000}},
        {{"radius", "far"}},
        {{"blade_height", {1.0, 0.5}}},
        {{"blade_width", {0.0, 0.1}}},
        {{"wind_direction", {0, 0}}},
        {{"wind_direction", {1, 2, 3}}},
        {{"wind_strength", 9}},
        {{"slope", {0, 120}}},
        {{"base_color", "#12345"}},
        {{"tip_color", {2, 0, 0}}},
        {{"lod_start", 1.5}},
        {{"terrain_only", "yes"}},
        {{"seed", -3}},
        {{"blade_hieght", {0.2, 0.5}}},
    };
    for (const json& c : cases) {
        EXPECT_FALSE(apply(c)) << "accepted " << c.dump();
        EXPECT_FALSE(r.all_of<s::Grass>(e)) << "a rejected update left a component: " << c.dump();
    }
    EXPECT_OK(apply({{"blade_hieght", {0.2, 0.5}}}, /*strict=*/false));
}

TEST_F(GrassCodec, Removes) {
    ASSERT_OK(apply(json::object()));
    EXPECT_TRUE(s::remove_component(r, e, "grass"));
    EXPECT_FALSE(r.all_of<s::Grass>(e));
}

} // namespace
