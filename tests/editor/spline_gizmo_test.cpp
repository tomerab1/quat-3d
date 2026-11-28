// The editor spline gizmo's edits (ROADMAP G18.3): inserting a point on the
// curve keeps its shape, deleting refuses to break the spline.

#ifdef ENGINE_EDITOR

#include <gtest/gtest.h>

#include "editor/spline_gizmo.hpp"
#include "engine/spline/queries.hpp"

namespace {

namespace sp = engine::spline;
using engine::scene::Spline;

[[nodiscard]] Spline make(sp::SplineKind kind, std::vector<glm::vec3> pts) {
    Spline s;
    s.kind = kind;
    for (const glm::vec3& p : pts) s.points.push_back({p, 2.0F, 0.0F, 0});
    return s;
}

TEST(EditorSplineGizmo, InsertingOnAPolylineSplitsTheSegment) {
    Spline s = make(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}});
    const sp::Curve c = *sp::Curve::build(s);
    engine::editor::spline_insert_point(s, c, 1.5F);
    ASSERT_EQ(s.points.size(), 4U);
    EXPECT_NEAR(s.points[2].position.z, 5.0F, 1e-5F);
    EXPECT_NEAR(sp::Curve::build(s)->length(), 20.0F, 1e-4F);
}

TEST(EditorSplineGizmo, InsertingOnABezierKeepsItsShape) {
    Spline s = make(sp::SplineKind::cubic_bezier, {{0, 0, 0}, {0, 0, 5}, {10, 0, 5}, {10, 0, 0}});
    const sp::Curve before = *sp::Curve::build(s);
    engine::editor::spline_insert_point(s, before, 0.3F);
    ASSERT_EQ(s.points.size(), 7U);
    const sp::Curve after = *sp::Curve::build(s);
    EXPECT_NEAR(after.length(), before.length(), 1e-3F);
    for (int i = 0; i <= 20; ++i) {
        const glm::vec3 p = before.position(static_cast<float>(i) / 20.0F);
        EXPECT_LE(sp::closest_point(after, p)->gap, 1e-4F);
    }
}

TEST(EditorSplineGizmo, DeletingNeverBreaksTheSpline) {
    Spline s = make(sp::SplineKind::centripetal_catmull_rom, {{0, 0, 0}, {5, 0, 1}, {10, 0, 0}});
    engine::editor::spline_delete_point(s, 1);
    EXPECT_EQ(s.points.size(), 2U);
    engine::editor::spline_delete_point(s, 0); // would leave one point: refused
    EXPECT_EQ(s.points.size(), 2U);

    Spline b = make(sp::SplineKind::cubic_bezier,
                    {{0, 0, 0}, {1, 0, 1}, {2, 0, 1}, {3, 0, 0}, {4, 0, -1}, {5, 0, -1}, {6, 0, 0}});
    engine::editor::spline_delete_point(b, 1); // a handle: ignored
    EXPECT_EQ(b.points.size(), 7U);
    engine::editor::spline_delete_point(b, 3); // the middle anchor and its handles
    ASSERT_EQ(b.points.size(), 4U);
    EXPECT_TRUE(sp::validate(b).has_value());
}

} // namespace

#endif // ENGINE_EDITOR
