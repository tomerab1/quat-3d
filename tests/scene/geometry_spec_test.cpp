#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/geometry_spec.hpp"
#include "support/expect_ok.hpp"

using nlohmann::json;
using engine::scene::generate_from_spec;

TEST(GeometrySpec, EveryGeneratorRuns) {
    const json roof = {{"type", "roof"},
                       {"polygon", {{0, 0}, {8, 0}, {8, 5}, {0, 5}}},
                       {"weights", {1, 0, 1, 0}},
                       {"base_height", 3.0},
                       {"height_scale", 0.8}};
    auto r = generate_from_spec(roof);
    ASSERT_OK(r);
    EXPECT_EQ(r->submeshes.size(), 2U); // slopes + gables
    const json sweep = {{"type", "sweep"},
                        {"path", {{0, 0, 0}, {4, 0, 0}, {6, 0, 3}}},
                        {"profile", {{-0.3, 0}, {0.3, 0}, {0.3, 1}, {-0.3, 1}}},
                        {"closed_profile", true}};
    EXPECT_OK(generate_from_spec(sweep));
    const json extrude = {{"type", "extrude"}, {"polygon", {{0, 0}, {4, 0}, {4, 3}, {0, 3}}}, {"height", 2.5},
                          {"taper", 0.2}};
    EXPECT_OK(generate_from_spec(extrude));
    const json stone = {{"type", "stone"}, {"seed", 42}, {"size", {0.5, 0.3, 0.4}}, {"chips", 4}};
    auto s = generate_from_spec(stone);
    ASSERT_OK(s);
    // Same spec, same mesh.
    auto again = generate_from_spec(stone);
    ASSERT_OK(again);
    EXPECT_EQ(engine::geometry::mesh_hash(*s), engine::geometry::mesh_hash(*again));
}

TEST(GeometrySpec, ErrorsNameTheField) {
    auto expect_error = [](const json& spec, const std::string& needle) {
        auto r = generate_from_spec(spec, "gen");
        ASSERT_FALSE(r.has_value()) << spec.dump();
        EXPECT_NE(r.error().message.find(needle), std::string::npos) << r.error().message;
    };
    expect_error(json::array(), "expected an object");
    expect_error({{"type", "castle"}}, "type");
    expect_error({{"type", "roof"}, {"polygon", {{0, 0}, {1, 0}}}}, "ring 0 has 2 points");
    expect_error({{"type", "roof"}, {"polygon", {{0, 0}, {0, 1}, {1, 1}, {1, 0}}}}, "counter-clockwise");
    expect_error({{"type", "roof"}, {"polygon", "square"}}, "polygon");
    expect_error({{"type", "roof"}, {"polygon", {{0, 0}, {1, 0}, {0, 1}}}, {"pitch", 30}}, "pitch");
    expect_error({{"type", "stone"}, {"subdivisions", 9}}, "subdivisions");
    expect_error({{"type", "sweep"}, {"path", {{0, 0, 0}}}, {"profile", {{0, 0}, {1, 0}}}}, "path");
    expect_error({{"type", "extrude"}, {"polygon", {{0, 0}, {1, 0}, {0, 1}}}, {"height", -1}}, "height");
    expect_error({{"type", "roof"}, {"polygon", {{0, 0}, {1, 0}, {0, 1}}}, {"base_height", "high"}}, "base_height");
    // Bounded: too many path x profile vertices is refused before generating.
    json path = json::array();
    for (int i = 0; i < 4000; ++i) path.push_back({i * 1.0, 0.0, 0.0});
    json profile = json::array();
    for (int i = 0; i < 300; ++i) profile.push_back({std::cos(i * 0.02), std::sin(i * 0.02)});
    expect_error({{"type", "sweep"}, {"path", path}, {"profile", profile}}, "vertex limit");
}
