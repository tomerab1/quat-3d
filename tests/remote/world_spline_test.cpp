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
