// The `spline` script table (ROADMAP G18.1): length, sampling with frames,
// closest point, and edits through the component codec.

#include "engine/scene/spline.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

TEST_F(ScriptTest, SplineQueriesAndCodecEdits) {
    write("path.lua", R"(
        function on_start(self)
            local e = self.entity
            e:set("spline", { kind = "polyline", follow_ground = false,
                              points = { vec3(0, 0, 0), vec3(10, 0, 0), { position = vec3(10, 0, 6), width = 3 } } })
            print("length", string.format("%.3f", spline.length(e)))
            local s = spline.sample(e, 4, { fit_to_ends = true, frames = "up_locked" })
            print("samples", #s, string.format("%.3f %.3f", s[2].position.x, s[2].right.z))
            local at = spline.at(e, 13)
            print("at", string.format("%.3f %.3f %.3f", at.position.x, at.position.z, at.width))
            local c = spline.closest(e, vec3(4, 7, 2), true)
            print("closest", string.format("%.3f %.3f", c.distance, c.gap))
            local ok, err = pcall(spline.sample, e, 0)
            print("bad", ok, err)
            ok, err = pcall(e.set, e, "spline", { closed = true, points = { vec3(0, 0, 0), vec3(1, 0, 0) } })
            print("invalid", ok, err)
            local stroke = {}
            for i = 0, 20 do stroke[#stroke + 1] = vec3(i * 0.5, 0, 0) end
            stroke[#stroke + 1] = { position = vec3(10, 0, 5), straight = true }
            local fitted = spline.fit(stroke, { output = "cubic_bezier" })
            e:set("spline", fitted)
            print("fit", fitted.kind, string.format("%.3f", spline.length(e)))
            ok, err = pcall(spline.fit, { vec3(0, 0, 0) })
            print("fit_bad", ok, err)
        end
    )");
    (void)scripted("path", "path.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "length\t16.000")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "samples\t5\t4.000 1.000")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "at\t10.000 3.000 2.000")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "closest\t4.000 2.000")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "bad\tfalse\tspline.sample: spacing must give at most 4096 samples"));
    EXPECT_TRUE(has_line(log, "invalid\tfalse\tspline.points: a closed polyline spline needs at least 3 points"))
        << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "fit\tcubic_bezier\t15.000")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "fit_bad\tfalse\tspline.fit: points: a stroke needs at least 2 points, got 1"));
}

} // namespace
} // namespace quat::test
