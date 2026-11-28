// spline.* world commands (ROADMAP G18.1).

#include <cmath>

#include "engine/scene/spline.hpp"
#include "world_fixture.hpp"

namespace quat::test {
namespace {

namespace rc = engine::remote::rpc_code;

class WorldSpline : public WorldCommands {
protected:
    void SetUp() override { register_commands(); }

    // A straight 10 m polyline along +X at height 1, entity moved by +5 in Z.
    Json make_line() {
        const Json made = ok("scene.create", {{"name", "wall"},
                                              {"components",
                                               {{"transform", {{"position", {0, 0, 5}}}},
                                                {"spline",
                                                 {{"kind", "polyline"},
                                                  {"follow_ground", false},
                                                  {"points", {{0, 1, 0}, {10, 1, 0}}}}}}}});
        return made["id"];
    }
};

TEST_F(WorldSpline, GetAndSetReportLengthAndBounds) {
    const Json id = make_line();
    Json out = ok("spline.get", {{"entity", id}});
    EXPECT_EQ(out["spline"]["kind"], "polyline");
    EXPECT_NEAR(out["length"].get<float>(), 10.0F, 1e-4F);
    EXPECT_EQ(out["segments"], 1);
    EXPECT_NEAR(out["bounds"]["min"][2].get<float>(), 4.5F, 1e-5F); // world z 5, half width 0.5

    out = ok("spline.set", {{"entity", id}, {"points", {{0, 1, 0}, {10, 1, 0}, {10, 1, 10}}}});
    EXPECT_NEAR(out["length"].get<float>(), 20.0F, 1e-4F);
    EXPECT_EQ(err("spline.set", {{"entity", id}, {"kind", "cubic_bezier"}}, rc::invalid_params),
              "spline.points: a cubic_bezier spline needs 3k + 1 points (anchor, handle, handle, anchor, ...), got 3");
    EXPECT_EQ(err("spline.set", {{"entity", id}, {"points", {{0, 1, 0}, {1, 1}}}}, rc::invalid_params),
              "spline.points[1]: expected [x, y, z] in metres (finite, within 1e6)");
    // The failed edits changed nothing.
    EXPECT_EQ(ok("spline.get", {{"entity", id}})["spline"]["points"].size(), 3U);

    const Json bare = ok("scene.create", {{"name", "bare"}})["id"];
    EXPECT_NE(err("spline.get", {{"entity", bare}}, rc::not_found).find("has no spline"), std::string::npos);
    err("spline.get", Json::object(), rc::invalid_params);
}

TEST_F(WorldSpline, SampleSpacingFramesAndLimits) {
    const Json id = make_line();
    Json out = ok("spline.sample", {{"entity", id}, {"spacing", 2.5}});
    ASSERT_EQ(out["samples"].size(), 5U);
    EXPECT_NEAR(out["samples"][4]["position"][0].get<float>(), 10.0F, 1e-5F);
    EXPECT_NEAR(out["samples"][4]["position"][2].get<float>(), 5.0F, 1e-5F);
    EXPECT_FALSE(out["samples"][0].contains("up"));
    out = ok("spline.sample", {{"entity", id}, {"spacing", 5}, {"frames", "rotation_minimising"}});
    EXPECT_TRUE(out["samples"][0].contains("right"));
    EXPECT_NE(err("spline.sample", {{"entity", id}, {"spacing", 5}, {"frames", "frenet"}}, rc::invalid_params)
                  .find("up_locked"),
              std::string::npos);
    EXPECT_NE(err("spline.sample", {{"entity", id}, {"spacing", 0.001}}, rc::invalid_params).find("spacing"),
              std::string::npos);
    // No terrain: draping is unavailable, and says how to skip it.
    EXPECT_NE(err("spline.sample", {{"entity", id}, {"ground", true}}, rc::unavailable).find("ground: false"),
              std::string::npos);
}

TEST_F(WorldSpline, ClosestCrossingsAndOffset) {
    const Json id = make_line();
    Json out = ok("spline.closest", {{"entity", id}, {"point", {4, 1, 8}}});
    EXPECT_NEAR(out["distance"].get<float>(), 4.0F, 1e-4F);
    EXPECT_NEAR(out["gap"].get<float>(), 3.0F, 1e-4F);
    EXPECT_NE(err("spline.closest", {{"entity", id}}, rc::invalid_params).find("point"), std::string::npos);

    const Json cross = ok("scene.create", {{"name", "cross"},
                                           {"components",
                                            {{"spline",
                                              {{"kind", "polyline"}, {"points", {{3, 0, 0}, {3, 0, 10}}}}}}}})["id"];
    out = ok("spline.crossings", {{"entity", id}, {"other", cross}});
    ASSERT_EQ(out["crossings"].size(), 1U);
    EXPECT_NEAR(out["crossings"][0]["position_a"][0].get<float>(), 3.0F, 1e-4F);
    EXPECT_NEAR(out["crossings"][0]["position_b"][1].get<float>(), 0.0F, 1e-4F);
    EXPECT_TRUE(ok("spline.crossings", {{"entity", id}})["crossings"].empty());

    out = ok("spline.offset", {{"entity", id}, {"distance", 2}, {"spacing", 5}});
    ASSERT_EQ(out["points"].size(), 3U);
    EXPECT_NEAR(out["points"][1][2].get<float>(), 7.0F, 1e-5F); // right of +X travel is +Z
    EXPECT_NE(err("spline.offset", {{"entity", id}}, rc::invalid_params).find("non-zero"), std::string::npos);
}

} // namespace
} // namespace quat::test

namespace quat::test {
namespace {

TEST_F(WorldSpline, NetworkReportsNodesAndEdges) {
    auto make = [this](const char* name, Json points) {
        return ok("scene.create", {{"name", name},
                                   {"components",
                                    {{"spline", {{"kind", "polyline"}, {"network", 3}, {"points", std::move(points)}}}}}})["id"];
    };
    const Json a = make("a", {{-5, 0, 0}, {5, 0, 0}});
    make("b", {{0, 0, -5}, {0, 0, 5}});
    const Json out = ok("spline.network", {{"network", 3}});
    ASSERT_EQ(out["edges"].size(), 4U);
    ASSERT_EQ(out["nodes"].size(), 5U);
    int crosses = 0;
    for (const Json& n : out["nodes"]) {
        EXPECT_EQ(n["id"].get<std::string>().size(), 16U);
        if (n["kind"] == "cross") {
            ++crosses;
            EXPECT_EQ(n["ends"].size(), 4U);
        }
    }
    EXPECT_EQ(crosses, 1);
    EXPECT_EQ(out["edges"][0]["entity"], a);

    EXPECT_EQ(err("spline.network", {{"network", 4}}, rc::not_found), "network 4: no spline has it");
    EXPECT_NE(err("spline.network", Json::object(), rc::invalid_params).find("network"), std::string::npos);
    err("spline.network", {{"network", 3}, {"snap_radius", 0}}, rc::invalid_params);
}

} // namespace
} // namespace quat::test

namespace quat::test {
namespace {

TEST_F(WorldSpline, FitTurnsAStrokeIntoASpline) {
    // A jagged freehand arc, then a straight segment drawn with the modifier.
    Json pts = Json::array();
    for (int i = 0; i <= 40; ++i) {
        const float a = 1.5707963F * static_cast<float>(i) / 40.0F;
        pts.push_back({10.0F * std::cos(a), 0.0F, 10.0F * std::sin(a) + ((i % 2) != 0 ? 0.02F : -0.02F)});
    }
    pts.push_back({{"position", {-5, 0, 10}}, {"straight", true}});
    Json out = ok("spline.fit", {{"points", pts}});
    EXPECT_FALSE(out["closed"].get<bool>());
    EXPECT_EQ(out["spline"]["kind"], "centripetal_catmull_rom");
    EXPECT_GE(out["pieces"].get<int>(), 2);
    EXPECT_LT(out["spline"]["points"].size(), 41U);

    // Written to an entity that sits at x = 100: its points are local.
    const Json id = ok("scene.create", {{"name", "path"}, {"components", {{"transform", {{"position", {100, 0, 0}}}}}}})["id"];
    out = ok("spline.fit", {{"points", pts}, {"entity", id}, {"output", "cubic_bezier"}});
    EXPECT_EQ(out["spline"]["kind"], "cubic_bezier");
    EXPECT_NEAR(out["spline"]["points"][0][0].get<float>(), 10.0F - 100.0F, 1e-3F);
    EXPECT_NEAR(out["bounds"]["max"][0].get<float>(), 110.5F, 0.2F); // world: back at x = 110 (+ half width)

    EXPECT_EQ(err("spline.fit", {{"points", {{0, 0, 0}}}}, rc::invalid_params),
              "points: a stroke needs at least 2 points, got 1");
    EXPECT_EQ(err("spline.fit", {{"points", {{0, 0, 0}, {1, 0}}}}, rc::invalid_params),
              "points[1]: expected [x, y, z] or {position: [x, y, z], straight?: bool}");
    EXPECT_NE(err("spline.fit", {{"points", pts}, {"output", "nurbs"}}, rc::invalid_params).find("catmull_rom"),
              std::string::npos);
    EXPECT_NE(err("spline.fit", Json::object(), rc::invalid_params).find("points"), std::string::npos);
}

} // namespace
} // namespace quat::test
