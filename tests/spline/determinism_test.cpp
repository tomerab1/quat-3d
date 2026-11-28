// Bit-exact determinism of engine_spline's outputs (ROADMAP G18): the same
// input must give the same bits on Linux, Windows and macOS (CI runs this on
// all three). The library uses only + - * / sqrt in a fixed order and is
// built without floating-point contraction; these hashes pin what games save
// or regenerate from: stroke fits, frames, networks, offsets, every kind.
// Inputs are literals (no libm: sin / cos may differ by an ulp per platform).
//
// A hash changes only with an intended algorithm change: then update the
// reference in the same commit and say why.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "engine/spline/frames.hpp"
#include "engine/spline/network.hpp"
#include "engine/spline/queries.hpp"
#include "engine/spline/stroke.hpp"

namespace quat::test {
namespace {

namespace sp = engine::spline;

struct Hasher {
    std::uint64_t h = 1469598103934665603ULL;
    void f(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h = (h ^ bits) * 1099511628211ULL;
    }
    void u(std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; }
    void v3(const glm::vec3& v) {
        f(v.x);
        f(v.y);
        f(v.z);
    }
};

[[nodiscard]] sp::Spline make(sp::SplineKind kind, std::vector<glm::vec3> pts, bool closed = false) {
    sp::Spline s;
    s.kind = kind;
    s.closed = closed;
    float w = 1.0F;
    for (const glm::vec3& p : pts) {
        s.points.push_back({p, w, 0.5F * w, 0});
        w += 0.25F;
    }
    return s;
}

[[nodiscard]] sp::Curve build(const sp::Spline& s) {
    auto c = sp::Curve::build(s);
    EXPECT_TRUE(c.has_value());
    return c ? *c : sp::Curve{};
}

void hash_samples(Hasher& h, const std::vector<sp::CurveSample>& samples) {
    for (const sp::CurveSample& s : samples) {
        h.v3(s.position);
        h.v3(s.tangent);
        h.f(s.width);
        h.f(s.height);
        h.f(s.param);
        h.f(s.distance);
        h.u(s.tags);
    }
}

#define EXPECT_HASH(hasher, reference) \
    EXPECT_EQ((hasher).h, reference##ULL) << "reference hash (update only for an intended change): " << (hasher).h

TEST(SplineDeterminism, EveryKindAndTransform) {
    Hasher h;
    glm::mat4 m(1.0F);
    m[0] = glm::vec4(0.8F, 0.0F, -0.6F, 0.0F); // a rotation about Y with exact entries
    m[2] = glm::vec4(0.6F, 0.0F, 0.8F, 0.0F);
    m[3] = glm::vec4(1234.5F, 7.25F, -321.75F, 1.0F);
    const sp::Spline kinds[] = {
        make(sp::SplineKind::cubic_bezier, {{0, 0, 0}, {2, 1, 3}, {5, -1, 4}, {7, 0, 1}, {9, 2, -2}, {11, 0, 0}, {13, 1, 2}}),
        make(sp::SplineKind::polyline, {{0, 0, 0}, {3.5F, 0.25F, 1}, {4, 1, 6.5F}, {-2, 0, 5}}, true),
        make(sp::SplineKind::centripetal_catmull_rom, {{0, 0, 0}, {1, 3, 2}, {4, 1, 5}, {6, 2, 1}, {3, 0, -2}}, true),
    };
    for (const sp::Spline& s : kinds) {
        const sp::Curve c = build(sp::transformed(s, m));
        hash_samples(h, sp::sample_by_spacing(c, {.spacing = 0.31F, .phase = 0.07F}));
        hash_samples(h, sp::sample_by_spacing(c, {.spacing = 0.9F, .fit_to_ends = true}));
        h.v3(c.bounds().min);
        h.v3(c.bounds().max);
        h.f(c.length());
    }
    EXPECT_HASH(h, 14148247619419745048);
}

TEST(SplineDeterminism, FramesOnA3DCurve) {
    Hasher h;
    const sp::Curve c = build(make(sp::SplineKind::centripetal_catmull_rom,
                                   {{0, 0, 0}, {3, 2, 1}, {5, 4, 4}, {3, 6, 7}, {0, 8, 6}, {-2, 10, 3}}));
    for (const sp::FrameMode mode : {sp::FrameMode::up_locked, sp::FrameMode::rotation_minimising}) {
        for (const sp::Frame& f : sp::sample_frames(c, {.spacing = 0.2F}, mode)) {
            h.v3(f.position);
            h.v3(f.tangent);
            h.v3(f.up);
            h.v3(f.right);
            h.f(f.width);
            h.f(f.distance);
        }
    }
    EXPECT_HASH(h, 15309088562037787655);
}

TEST(SplineDeterminism, StrokeFitsInBothOutputs) {
    // A stroke with jitter from integer arithmetic, a straight segment and a
    // closing loop; then an open one.
    // Each statement rounds once (no a * b + c in one expression): this test
    // file is compiled with the default contraction, which may fuse those.
    std::vector<sp::StrokePoint> loop;
    for (int i = 0; i < 160; ++i) {
        const int k = i % 40;
        const float jitter = static_cast<float>((i * 37) % 11 - 5) * 0.004F;
        const float along = static_cast<float>(k) * 0.25F;
        const float back = 10.0F - along;
        glm::vec3 p;
        if (i < 40) {
            p = {along, 0.0F, jitter};
        } else if (i < 80) {
            const float x = 10.0F + jitter;
            p = {x, 0.0F, along};
        } else if (i < 120) {
            const float z = 10.0F + jitter;
            p = {back, 0.0F, z};
        } else {
            p = {jitter, 0.0F, back};
        }
        loop.push_back({p, false});
    }
    loop.push_back({{0.1F, 0.0F, 0.2F}, false});
    std::vector<sp::StrokePoint> open;
    for (int i = 0; i <= 60; ++i) {
        const float t = static_cast<float>(i) * 0.2F;
        const float bend = static_cast<float>(i * i) * 0.002F;
        const float jitter = static_cast<float>((i * 13) % 7 - 3) * 0.003F;
        const float z = bend + jitter;
        open.push_back({{t, 0.0F, z}, false});
    }
    open.push_back({{20.0F, 0.0F, 7.2F}, true});
    Hasher h;
    for (const auto* stroke : {&loop, &open}) {
        for (const sp::StrokeOutput out : {sp::StrokeOutput::catmull_rom, sp::StrokeOutput::cubic_bezier}) {
            sp::StrokeOptions o;
            o.output = out;
            const auto fit = sp::fit_stroke(*stroke, o);
            ASSERT_TRUE(fit.has_value()) << fit.error().message;
            h.u(fit->closed ? 1U : 0U);
            h.u(fit->spline.points.size());
            for (const sp::SplinePoint& p : fit->spline.points) h.v3(p.position);
        }
    }
    EXPECT_HASH(h, 16218459374671512726);
}

TEST(SplineDeterminism, NetworkAndOffset) {
    const sp::Curve wall = build(make(sp::SplineKind::centripetal_catmull_rom,
                                      {{0, 0, 0}, {8, 0, 2.5F}, {16, 0, 0}, {24, 0, 3}}));
    const sp::Curve path = build(make(sp::SplineKind::polyline, {{12, 0, -6}, {12.5F, 0, 9}}));
    const sp::Curve spur = build(make(sp::SplineKind::polyline, {{3.9F, 0, 1.7F}, {3, 0, 9}})); // starts 0.2 m off the wall: a T
    const std::vector<sp::NetworkCurve> in = {{3, &spur}, {1, &wall}, {2, &path}};
    auto net = sp::build_network(in);
    ASSERT_TRUE(net.has_value());
    Hasher h;
    for (const sp::NetworkNode& n : net->nodes) {
        h.u(n.id);
        h.v3(n.position);
        h.u(static_cast<std::uint64_t>(n.kind));
        for (const sp::NodeEnd& e : n.ends) {
            h.u(e.edge);
            h.f(e.direction.x);
            h.f(e.direction.y);
        }
    }
    for (const sp::NetworkEdge& e : net->edges) {
        h.u(e.id);
        h.f(e.t0);
        h.f(e.t1);
        h.f(e.length);
    }
    for (const glm::vec3& p : sp::offset_xz(wall, {.distance = 1.75F, .width_factor = 0.5F, .spacing = 0.3F})) h.v3(p);
    for (const glm::vec3& p : sp::offset_xz(wall, {.distance = -6.0F, .spacing = 0.3F})) h.v3(p); // trims
    // The fixture: 5 ends, a tee (the spur) and a cross (the path).
    EXPECT_EQ(net->nodes.size(), 7U);
    int tees = 0;
    int crosses = 0;
    for (const sp::NetworkNode& n : net->nodes) {
        tees += n.kind == sp::NodeKind::tee ? 1 : 0;
        crosses += n.kind == sp::NodeKind::cross ? 1 : 0;
    }
    EXPECT_EQ(tees, 1);
    EXPECT_EQ(crosses, 1);
    EXPECT_HASH(h, 13929671024108074034);
}

} // namespace
} // namespace quat::test
