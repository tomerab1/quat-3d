// G7: the "script" component in the codec — validation of paths and
// property names, typed properties, merge semantics, and scene-file round
// trips.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/script_component.hpp"
#include "support/expect_ok.hpp"

namespace {

using namespace engine::scene;
using json = nlohmann::json;

TEST(ScriptComponent, PathsAreConfinedToTheScriptsDirectory) {
    EXPECT_OK(validate_script_path("coin.lua"));
    EXPECT_OK(validate_script_path("enemies/grunt-1.lua"));
    for (const char* bad : {"", "coin", "coin.txt", "../coin.lua", "a/../../b.lua", "/abs.lua",
                            "a//b.lua", "./coin.lua", "a\\b.lua", "spaced name.lua", "c:x.lua"}) {
        EXPECT_ERR(validate_script_path(bad)) << bad;
    }
    EXPECT_OK(validate_property_name("speed_2"));
    for (const char* bad : {"", "2fast", "with space", "entity", "a-b"}) {
        EXPECT_ERR(validate_property_name(bad)) << bad;
    }
}

TEST(ScriptComponent, CodecRoundTripsTypedProperties) {
    Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity e = scene.create_entity("coin");
    ASSERT_OK(apply_component(r, e, "script",
                              json{{"path", "coin.lua"},
                                   {"properties",
                                    {{"value", 5}, {"label", "gold"}, {"spin", true},
                                     {"axis", json::array({0, 1, 0})}}}},
                              true));
    const Script& s = r.get<Script>(e);
    EXPECT_EQ(s.path, "coin.lua");
    EXPECT_TRUE(s.enabled);
    ASSERT_EQ(s.properties.size(), 4U);
    const std::uint32_t rev = s.revision;

    const json out = encode_components(r, e)["script"];
    EXPECT_EQ(out["path"], "coin.lua");
    EXPECT_EQ(out["properties"]["value"], 5.0);
    EXPECT_EQ(out["properties"]["label"], "gold");
    EXPECT_EQ(out["properties"]["spin"], true);
    EXPECT_EQ(out["properties"]["axis"], json::array({0.0, 1.0, 0.0}));

    // Merge: one property changes, null removes another, the rest stay.
    ASSERT_OK(apply_component(r, e, "script",
                              json{{"properties", {{"value", 7}, {"label", nullptr}}}}, true));
    const json merged = encode_components(r, e)["script"]["properties"];
    EXPECT_EQ(merged["value"], 7.0);
    EXPECT_FALSE(merged.contains("label"));
    EXPECT_TRUE(merged.contains("spin"));
    EXPECT_NE(r.get<Script>(e).revision, rev) << "edits bump the revision";

    // A copy through the codec (scene files do exactly this) is identical.
    const entt::entity copy = scene.create_entity("copy");
    ASSERT_OK(apply_component(r, copy, "script", encode_components(r, e)["script"], false));
    EXPECT_EQ(encode_components(r, copy)["script"], encode_components(r, e)["script"]);
}

TEST(ScriptComponent, BadValuesAreRejectedWithPreciseErrors) {
    Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity e = scene.create_entity("x");
    const auto msg = [&](const json& v) {
        auto ok = apply_component(r, e, "script", v, true);
        return ok ? std::string() : ok.error().message;
    };
    EXPECT_NE(msg(json::object()).find("script.path: required"), std::string::npos);
    EXPECT_NE(msg(json{{"path", "../x.lua"}}).find("script.path:"), std::string::npos);
    EXPECT_NE(msg(json{{"path", "a.lua"}, {"properties", {{"bad name", 1}}}}).find("identifiers"),
              std::string::npos);
    EXPECT_NE(msg(json{{"path", "a.lua"}, {"properties", {{"v", json::object()}}}})
                  .find("script.properties.v: expected a number"),
              std::string::npos);
    EXPECT_NE(msg(json{{"path", "a.lua"}, {"enabeld", true}}).find("unknown field"),
              std::string::npos);
    EXPECT_FALSE(r.all_of<Script>(e)) << "failed adds leave nothing behind";
    EXPECT_TRUE(remove_component(r, e, "script") == false);
}

} // namespace
