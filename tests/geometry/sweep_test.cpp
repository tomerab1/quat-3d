#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "engine/geometry/sweep.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
namespace asset = engine::asset;
using quat::test::geometry::signed_volume;

namespace {

const std::vector<glm::dvec2> kSquareProfile{{-0.5, 0.0}, {0.5, 0.0}, {0.5, 1.0}, {-0.5, 1.0}};

// Distance from p to the line through a-b, if p projects within the
// segment extended by `margin` at both ends; infinity otherwise.
double distance_to_wall_line(glm::dvec3 p, glm::dvec3 a, glm::dvec3 b, double margin) {
    const glm::dvec3 ab = b - a;
    const double len = glm::length(ab);
    const double s = glm::dot(p - a, ab) / len;
    if (s < -margin || s > len + margin) {
        return INFINITY;
    }
    return glm::length(p - (a + ab * (s / len)));
}

} // namespace

TEST(GeometryFrames, RotationMinimisingOnAPlanarPath) {
    // A horizontal path: the binormal stays up, the frames never twist.
    std::vector<glm::dvec3> path;
    for (int i = 0; i <= 50; ++i) {
        const double a = i * 0.1;
        path.push_back({a * 2.0, 0.0, std::sin(a) * 3.0});
    }
    auto frames = path_frames(path);
    ASSERT_OK(frames);
    ASSERT_EQ(frames->size(), path.size());
    for (const PathFrame& f : *frames) {
        EXPECT_NEAR(f.binormal.y, 1.0, 1e-12);
        EXPECT_NEAR(glm::dot(f.normal, f.tangent), 0.0, 1e-12);
        EXPECT_NEAR(glm::length(f.normal), 1.0, 1e-12);
    }
    EXPECT_DOUBLE_EQ((*frames)[0].distance, 0.0);
    EXPECT_GT(frames->back().distance, 10.0);
}

TEST(GeometryFrames, ClosedHelixLoopHasNoSeamTwist) {
    std::vector<glm::dvec3> loop;
    for (int i = 0; i < 64; ++i) {
        const double a = 6.283185307179586 * i / 64.0;
        loop.push_back({std::cos(a) * 4.0, std::sin(2.0 * a), std::sin(a) * 4.0}); // a saddle loop
    }
    FrameOptions options;
    options.closed = true;
    auto frames = path_frames(loop, options);
    ASSERT_OK(frames);
    // Neighbouring frames (including across the seam) differ by a small
    // rotation only.
    for (std::size_t i = 0; i < frames->size(); ++i) {
        const PathFrame& a = (*frames)[i];
        const PathFrame& b = (*frames)[(i + 1) % frames->size()];
        EXPECT_GT(glm::dot(a.normal, b.normal), 0.95) << i;
    }
}

TEST(GeometryFrames, RefusesBadPaths) {
    EXPECT_ERR(path_frames(std::vector<glm::dvec3>{{0, 0, 0}}));
    EXPECT_ERR(path_frames(std::vector<glm::dvec3>{{0, 0, 0}, {0, 0, 0}, {1, 0, 0}}));
    EXPECT_ERR(path_frames(std::vector<glm::dvec3>{{0, 0, 0}, {1, 0, 0}, {0, 0, 0.001}})); // U-turn
    EXPECT_ERR(path_frames(std::vector<glm::dvec3>{{0, 0, 0}, {1, 0, NAN}}));
}

TEST(GeometrySweep, StraightWallIsABox) {
    auto frames = path_frames(std::vector<glm::dvec3>{{0, 0, 0}, {4, 0, 0}});
    ASSERT_OK(frames);
    SweepOptions options;
    options.closed_profile = true;
    options.cap_start = options.cap_end = true;
    options.cap_material = 2;
    auto wall = sweep(kSquareProfile, *frames, options);
    ASSERT_OK(wall);
    ASSERT_OK(validate(*wall));
    EXPECT_NEAR(signed_volume(*wall), 4.0, 1e-5); // closed and outward
    EXPECT_EQ(wall->submeshes.size(), 2U);
    EXPECT_EQ(wall->submeshes[1].material, 2U);
    const asset::Aabb box = compute_bounds(*wall);
    EXPECT_FLOAT_EQ(box.min.y, 0.0F);
    EXPECT_FLOAT_EQ(box.max.y, 1.0F);
    EXPECT_FLOAT_EQ(box.min.z, -0.5F);
    EXPECT_FLOAT_EQ(box.max.z, 0.5F);
    // UVs: u = metres along the path.
    float max_u = 0.0F;
    for (const asset::Vertex& v : wall->vertices) {
        max_u = std::max(max_u, v.uv.x);
    }
    EXPECT_FLOAT_EQ(max_u, 4.0F);
}

TEST(GeometrySweep, MitredCornerKeepsThickness) {
    const std::vector<glm::dvec3> path{{0, 0, 0}, {4, 0, 0}, {4, 0, 4}};
    auto frames = path_frames(path);
    ASSERT_OK(frames);
    EXPECT_NEAR((*frames)[1].miter_scale, std::sqrt(2.0), 1e-12);
    SweepOptions options;
    options.closed_profile = true;
    options.cap_start = options.cap_end = true;
    auto wall = sweep(kSquareProfile, *frames, options);
    ASSERT_OK(wall);
    EXPECT_NEAR(signed_volume(*wall), 8.0, 1e-4); // centre line 8 m, 1 m^2 section
    // Every vertex is 0.5 m from the centre line of a segment it belongs
    // to: the mitre stretch keeps the wall's thickness through the corner
    // (the corner points lie 0.5 m from both centre lines).
    for (const asset::Vertex& v : wall->vertices) {
        const glm::dvec3 p(v.position.x, 0.0, v.position.z);
        const double d = std::min(distance_to_wall_line(p, path[0], path[1], 0.5),
                                  distance_to_wall_line(p, path[1], path[2], 0.5));
        EXPECT_NEAR(d, 0.5, 1e-5);
    }
}

TEST(GeometrySweep, ClosedPathTube) {
    std::vector<glm::dvec3> loop;
    for (int i = 0; i < 48; ++i) {
        const double a = 6.283185307179586 * i / 48.0;
        loop.push_back({std::cos(a) * 5.0, 0.0, -std::sin(a) * 5.0});
    }
    FrameOptions fo;
    fo.closed = true;
    auto frames = path_frames(loop, fo);
    ASSERT_OK(frames);
    SweepOptions options;
    options.closed_path = true;
    options.closed_profile = true;
    auto ring = sweep(kSquareProfile, *frames, options);
    ASSERT_OK(ring);
    ASSERT_OK(validate(*ring));
    EXPECT_GT(std::fabs(signed_volume(*ring)), 0.9 * 2.0 * 3.14159 * 5.0);
}

TEST(GeometrySweep, LoftAndErrors) {
    auto frames = path_frames(std::vector<glm::dvec3>{{0, 0, 0}, {0, 0, 2}, {0, 0, 4}});
    ASSERT_OK(frames);
    const std::vector<std::vector<glm::dvec2>> profiles{
        kSquareProfile, {{-0.25, 0}, {0.25, 0}, {0.25, 0.5}, {-0.25, 0.5}}, kSquareProfile};
    SweepOptions options;
    options.closed_profile = true;
    EXPECT_OK(loft(profiles, *frames, options));
    const std::vector<std::vector<glm::dvec2>> mismatched{kSquareProfile, {{0, 0}, {1, 0}, {0, 1}}, kSquareProfile};
    EXPECT_ERR(loft(mismatched, *frames, options));
    EXPECT_ERR(loft(std::span(profiles).first(2), *frames, options));
    auto sharp = path_frames(std::vector<glm::dvec3>{{0, 0, 0}, {4, 0, 0}, {0, 0, 0.5}});
    ASSERT_OK(sharp);
    EXPECT_ERR(sweep(kSquareProfile, *sharp, options)); // past the miter limit
}

TEST(GeometryExtrude, PrismWithHoleAndTaper) {
    const Polygon p{quat::test::geometry::rect(0, 0, 4, 4), {{{1, 1}, {1, 3}, {3, 3}, {3, 1}}}};
    ExtrudeOptions options;
    options.height = 2.0;
    auto prism = extrude(p, options);
    ASSERT_OK(prism);
    ASSERT_OK(validate(*prism));
    EXPECT_NEAR(signed_volume(*prism), 12.0 * 2.0, 1e-5);
    options.taper = 0.25; // the hole widens, the outline narrows
    auto tapered = extrude(p, options);
    ASSERT_OK(tapered);
    const double bottom = 12.0;
    const double top = 3.5 * 3.5 - 2.5 * 2.5;
    // Prismatoid: h/6 (A0 + 4 Amid + A1); midsection 3.75^2 - 2.25^2.
    const double mid = 3.75 * 3.75 - 2.25 * 2.25;
    EXPECT_NEAR(signed_volume(*tapered), 2.0 / 6.0 * (bottom + 4.0 * mid + top), 1e-4);
    options.taper = 0.5; // the courtyard wall is 1 m thick: the top folds
    EXPECT_ERR(extrude(p, options));
    options.taper = 0.0;
    options.height = -1.0;
    EXPECT_ERR(extrude(p, options));
    EXPECT_ERR(extrude(Polygon{{{0, 0}, {0, 1}, {1, 1}}, {}}, ExtrudeOptions{})); // clockwise
}
