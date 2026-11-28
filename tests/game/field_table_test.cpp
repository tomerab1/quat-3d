// Field-table components (ROADMAP G17.1): a component described field by
// field gets a codec with range checks, a JSON Schema and inspector metadata,
// with no per-component code.

#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/game/module_set.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/component_registry.hpp"
#include "support/expect_ok.hpp"

namespace {

using nlohmann::json;
namespace game = engine::game;
namespace scene = engine::scene;

struct Lantern {
    float        range = 6.0F;
    std::int32_t wicks = 1;
    bool         lit = true;
    glm::vec2    sway{0.0F};
    glm::vec3    offset{0.0F, 2.0F, 0.0F};
    glm::vec3    tint{1.0F, 0.8F, 0.5F};
    glm::vec4    glass{1.0F};
    std::string  label = "lamp";
    std::int32_t style = 0;
    float        burn_s = 0.0F; // runtime state
};

std::expected<void, engine::core::Error> lantern_module(game::Registrar& r) {
    r.component<Lantern>("test_lantern", "A lantern",
                         game::ComponentFields<Lantern>()
                             .field("range", &Lantern::range, {.min = 0.5, .max = 50.0, .description = "Radius, m"})
                             .field("wicks", &Lantern::wicks, {.min = 1, .max = 4})
                             .field("lit", &Lantern::lit)
                             .field("sway", &Lantern::sway, {.min = -1, .max = 1})
                             .field("offset", &Lantern::offset)
                             .color("tint", &Lantern::tint)
                             .field("glass", &Lantern::glass)
                             .field("label", &Lantern::label, {.max_length = 8})
                             .choice("style", &Lantern::style, {"iron", "brass"})
                             .field("burn_s", &Lantern::burn_s, {.read_only = true}));
    return {};
}

class FieldTable : public ::testing::Test {
protected:
    void SetUp() override {
        const game::ModuleEntry modules[] = {{"lanterns", &lantern_module}};
        auto loaded = game::ModuleSet::load(modules, scene::component_registry(), {});
        ASSERT_OK(loaded);
    }
    void TearDown() override { scene::component_registry().remove("test_lantern"); }

    entt::registry r;
    entt::entity   e = r.create();
};

TEST_F(FieldTable, DecodesEncodesAndRoundTrips) {
    ASSERT_OK(scene::apply_component(r, e, "test_lantern",
                                     {{"range", 12}, {"wicks", 3}, {"lit", false}, {"sway", {0.5, -0.5}},
                                      {"tint", "#ffffff"}, {"glass", {0.1, 0.2, 0.3, 0.4}},
                                      {"label", "porch"}, {"style", "brass"}},
                                     true));
    const Lantern& l = r.get<Lantern>(e);
    EXPECT_FLOAT_EQ(l.range, 12.0F);
    EXPECT_EQ(l.wicks, 3);
    EXPECT_FALSE(l.lit);
    EXPECT_FLOAT_EQ(l.tint.x, 1.0F); // sRGB white is linear white
    EXPECT_EQ(l.style, 1);
    const json j = scene::encode_components(r, e)["test_lantern"];
    EXPECT_EQ(j["style"], "brass");
    EXPECT_EQ(j["label"], "porch");
    EXPECT_EQ(j["offset"], json::array({0.0, 2.0, 0.0}));
    EXPECT_EQ(j["burn_s"], 0.0);
    // The encoded form decodes back to the same component (read-only echoed).
    const entt::entity copy = r.create();
    ASSERT_OK(scene::apply_component(r, copy, "test_lantern", j, true));
    EXPECT_EQ(scene::encode_components(r, copy)["test_lantern"], j);
}

TEST_F(FieldTable, ChecksEveryField) {
    const auto err = [&](const json& v) {
        auto ok = scene::apply_component(r, e, "test_lantern", v, true);
        return ok ? std::string() : ok.error().message;
    };
    EXPECT_NE(err({{"range", 0.1}}).find("test_lantern.range"), std::string::npos);
    EXPECT_NE(err({{"wicks", 9}}).find("test_lantern.wicks"), std::string::npos);
    EXPECT_NE(err({{"sway", {2, 0}}}).find("test_lantern.sway"), std::string::npos);
    EXPECT_NE(err({{"label", "much too long"}}).find("test_lantern.label"), std::string::npos);
    EXPECT_NE(err({{"style", "glass"}}).find("expected one of \"iron\", \"brass\""), std::string::npos);
    EXPECT_NE(err({{"tint", "red"}}).find("test_lantern.tint"), std::string::npos);
    EXPECT_NE(err({{"unknown", 1}}).find("unknown field"), std::string::npos);
    // A rejected update leaves the component as it was.
    ASSERT_OK(scene::apply_component(r, e, "test_lantern", {{"range", 7}}, true));
    EXPECT_FALSE(scene::apply_component(r, e, "test_lantern", {{"range", 9}, {"wicks", 0}}, true));
    EXPECT_FLOAT_EQ(r.get<Lantern>(e).range, 7.0F);
}

TEST_F(FieldTable, PublishesSchemaAndFields) {
    const scene::ComponentCodec* c = scene::component_registry().find("test_lantern");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->schema);
    ASSERT_TRUE(c->fields);
    const json& s = *c->schema;
    EXPECT_EQ(s["additionalProperties"], false);
    EXPECT_EQ(s["properties"]["range"]["minimum"], 0.5);
    EXPECT_EQ(s["properties"]["range"]["description"], "Radius, m");
    EXPECT_EQ(s["properties"]["wicks"]["type"], "integer");
    EXPECT_EQ(s["properties"]["style"]["enum"], json::array({"iron", "brass"}));
    EXPECT_EQ(s["properties"]["burn_s"]["readOnly"], true);
    EXPECT_EQ(c->fields->fields.size(), 10U);
    EXPECT_EQ(c->fields->fields[5].kind, scene::FieldKind::color3);
    ASSERT_OK(scene::apply_component(r, e, "test_lantern", json::object(), true));
    EXPECT_EQ(c->fields->component(r, e), static_cast<void*>(&r.get<Lantern>(e)));
}

} // namespace
