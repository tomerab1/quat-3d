// Cascaded shadow map fitting (R12), CPU reference: practical split scheme,
// depth-range quantisation, slice containment, and the stability properties
// (constant texel size under rotation, whole-texel shifts under translation).

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/csm.hpp"

namespace {

namespace r = engine::renderer;

constexpr std::uint32_t k_map = 2048;

glm::mat4 projection(float near_z = 0.1F, float far_z = 500.0F) {
    glm::mat4 p = glm::perspective(glm::radians(60.0F), 16.0F / 9.0F, near_z, far_z);
    p[1][1] *= -1.0F;
    return p;
}

r::CsmView make_view(const glm::vec3& eye, const glm::vec3& forward) {
    r::CsmView v;
    v.view = glm::lookAt(eye, eye + forward, glm::vec3(0.0F, 1.0F, 0.0F));
    v.projection = projection();
    v.to_light = glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F));
    return v;
}

// Shadow-map texel coordinate of `p` in cascade `c`.
glm::vec2 texel_of(const r::GpuCascades& cs, int c, const glm::vec3& p) {
    const glm::vec4 clip = cs.view_proj[static_cast<std::size_t>(c)] * glm::vec4(p, 1.0F);
    return (glm::vec2(clip) * 0.5F + 0.5F) * static_cast<float>(k_map);
}

// World-space corners of the view-frustum slice between view distances a..b.
std::array<glm::vec3, 8> slice_corners(const r::CsmView& v, float a, float b) {
    const glm::mat4 inv_vp = glm::inverse(v.projection * v.view);
    const glm::mat4 inv_view = glm::inverse(v.view);
    std::array<glm::vec3, 8> out{};
    int k = 0;
    for (float d : {a, b}) {
        for (glm::vec2 ndc : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(-1, 1),
                              glm::vec2(1, 1)}) {
            const glm::vec4 n = inv_vp * glm::vec4(ndc, 0.0F, 1.0F);
            const glm::vec4 f = inv_vp * glm::vec4(ndc, 1.0F, 1.0F);
            const glm::vec3 pn = glm::vec3(n) / n.w;
            const glm::vec3 pf = glm::vec3(f) / f.w;
            const glm::vec3 eye = glm::vec3(inv_view[3]);
            const glm::vec3 fwd = -glm::vec3(inv_view[2]);
            const float dn = glm::dot(pn - eye, fwd);
            const float df = glm::dot(pf - eye, fwd);
            out[static_cast<std::size_t>(k++)] = glm::mix(pn, pf, (d - dn) / (df - dn));
        }
    }
    return out;
}

} // namespace

TEST(CsmSplits, LinearLogAndPracticalBlend) {
    const auto lin = r::cascade_splits(1.0F, 100.0F, 4, 0.0F);
    const auto log = r::cascade_splits(1.0F, 100.0F, 4, 1.0F);
    const auto mid = r::cascade_splits(1.0F, 100.0F, 4, 0.5F);
    EXPECT_FLOAT_EQ(lin[0], 1.0F);
    EXPECT_FLOAT_EQ(lin[4], 100.0F);
    EXPECT_NEAR(lin[2], 50.5F, 1e-3F);
    EXPECT_NEAR(log[2], 10.0F, 1e-3F); // geometric mean
    EXPECT_NEAR(log[1], std::pow(100.0F, 0.25F), 1e-3F);
    for (int i = 1; i <= 3; ++i) {
        EXPECT_NEAR(mid[static_cast<std::size_t>(i)],
                    0.5F * (lin[static_cast<std::size_t>(i)] + log[static_cast<std::size_t>(i)]),
                    1e-3F);
        EXPECT_LT(mid[static_cast<std::size_t>(i)], mid[static_cast<std::size_t>(i + 1)]);
    }
}

TEST(CsmSplits, FewerCascadesEndAtFar) {
    const auto s = r::cascade_splits(0.5F, 80.0F, 2, 0.8F);
    EXPECT_FLOAT_EQ(s[0], 0.5F);
    EXPECT_FLOAT_EQ(s[2], 80.0F);
    EXPECT_FLOAT_EQ(s[4], 80.0F);
}

TEST(CsmRange, QuantisationIsOctaveRelativeAndConservative) {
    EXPECT_FLOAT_EQ(r::quantize_up(10.0F), 10.0F);   // 8 + 2 * (8 / 8)
    EXPECT_FLOAT_EQ(r::quantize_up(10.1F), 11.0F);
    EXPECT_FLOAT_EQ(r::quantize_down(10.9F), 10.0F);
    EXPECT_FLOAT_EQ(r::quantize_up(0.3F), 0.3125F);  // step 1/32 in [0.25, 0.5)
    for (float v : {0.07F, 1.3F, 17.0F, 333.0F, 4100.0F}) {
        EXPECT_GE(r::quantize_up(v), v);
        EXPECT_LE(r::quantize_down(v), v);
        EXPECT_LE(r::quantize_up(v), v * 1.126F);
    }
}

TEST(CsmRange, DepthBoundsTightenTheRange) {
    const glm::mat4 proj = projection();
    r::CsmSettings s;
    s.max_distance = 150.0F;
    const glm::vec2 fixed = r::shadow_range(proj, s, false, 0.0F, 0.0F).range;
    EXPECT_NEAR(fixed.x, 0.1F, 1e-4F);
    EXPECT_NEAR(fixed.y, 150.0F, 1e-3F);

    // Geometry between 5 m and 30 m from the camera.
    const auto depth_at = [&](float dist) {
        const glm::vec4 c = proj * glm::vec4(0.0F, 0.0F, -dist, 1.0F);
        return c.z / c.w;
    };
    const glm::vec2 tight =
        r::shadow_range(proj, s, true, depth_at(5.0F), depth_at(30.0F)).range;
    EXPECT_LE(tight.x, 5.0F);
    EXPECT_GE(tight.x, 4.0F);
    EXPECT_GE(tight.y, 30.0F);
    EXPECT_LE(tight.y, 32.0F);

    s.gpu_depth_bounds = false; // the CPU fallback ignores the bounds
    EXPECT_EQ(r::shadow_range(proj, s, true, depth_at(5.0F), depth_at(30.0F)).range, fixed);
    // Invalid bounds (no geometry: min > max) fall back to the full range.
    s.gpu_depth_bounds = true;
    EXPECT_EQ(r::shadow_range(proj, s, true, 1.0F, 0.0F).range, fixed);
}

TEST(CsmFit, MatchesGlmOrthoTimesLookAt) {
    const r::CsmView v = make_view(glm::vec3(3.0F, 2.0F, 1.0F), glm::vec3(0.0F, -0.2F, -1.0F));
    r::CsmSettings s;
    const r::GpuCascades cs = r::fit_cascades(v, s, glm::vec2(0.1F, 100.0F), k_map);
    // Rebuild cascade 2 with glm from the same light basis and extent.
    const glm::vec3 fwd = -v.to_light;
    const glm::mat4 light_view = glm::lookAt(glm::vec3(0.0F), fwd, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 inv = glm::inverse(cs.view_proj[2] * glm::inverse(light_view));
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(-1.0F, -1.0F, 0.0F, 1.0F));
    const glm::vec3 hi = glm::vec3(inv * glm::vec4(1.0F, 1.0F, 1.0F, 1.0F));
    const glm::mat4 ortho = glm::ortho(lo.x, hi.x, lo.y, hi.y, -lo.z, -hi.z);
    const glm::mat4 want = ortho * light_view;
    for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_NEAR(cs.view_proj[2][c][k], want[c][k], 1e-4F);
    }
}

TEST(CsmFit, EachSliceFitsInsideItsCascade) {
    const r::CsmView v = make_view(glm::vec3(10.0F, 4.0F, -3.0F), glm::vec3(0.6F, -0.3F, -0.7F));
    r::CsmSettings s;
    s.blend_band = 0.2F;
    const glm::vec2 range(0.1F, 150.0F);
    const r::GpuCascades cs = r::fit_cascades(v, s, range, k_map);
    EXPECT_EQ(cs.params.x, 4.0F);
    const auto splits = r::cascade_splits(range.x, range.y, 4, s.split_lambda);
    for (int c = 0; c < 4; ++c) {
        EXPECT_FLOAT_EQ(cs.split_far[c], splits[static_cast<std::size_t>(c + 1)]);
        float start = splits[static_cast<std::size_t>(c)];
        if (c > 0) {
            const auto i = static_cast<std::size_t>(c);
            start -= s.blend_band * (splits[i] - splits[i - 1]);
        }
        for (const glm::vec3& p :
             slice_corners(v, start, splits[static_cast<std::size_t>(c + 1)])) {
            const glm::vec4 clip = cs.view_proj[static_cast<std::size_t>(c)] * glm::vec4(p, 1.0F);
            EXPECT_LE(std::abs(clip.x), 1.0001F) << "cascade " << c;
            EXPECT_LE(std::abs(clip.y), 1.0001F) << "cascade " << c;
            EXPECT_GE(clip.z, 0.0F) << "cascade " << c;
            EXPECT_LE(clip.z, 1.0F) << "cascade " << c;
        }
        // Cascades grow with distance.
        if (c > 0) EXPECT_GT(cs.texel_world[c], cs.texel_world[c - 1]);
    }
}

// Without depth clamp the depth range is padded so casters toward the sun
// stay inside it.
TEST(CsmFit, PaddedRangeHoldsCastersTowardTheSun) {
    const r::CsmView v = make_view(glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F));
    r::CsmSettings s;
    s.caster_distance = 100.0F;
    const r::GpuCascades cs =
        r::fit_cascades(v, s, glm::vec2(0.1F, 60.0F), k_map, /*pancaked=*/false);
    // A point inside cascade 0, and a caster 80 m toward the sun from it.
    const glm::vec3 receiver(0.0F, 0.0F, -2.0F);
    const glm::vec3 caster = receiver + v.to_light * 80.0F;
    const glm::vec4 rc = cs.view_proj[0] * glm::vec4(receiver, 1.0F);
    const glm::vec4 cc = cs.view_proj[0] * glm::vec4(caster, 1.0F);
    EXPECT_GT(cc.z, 0.0F);
    EXPECT_LT(cc.z, rc.z);                 // nearer the light
    EXPECT_NEAR(cc.x, rc.x, 1e-4F);        // same texel column / row
    EXPECT_NEAR(cc.y, rc.y, 1e-4F);
    EXPECT_EQ(cs.limits.y, 0.0F);          // nothing reaches past depth 0
    EXPECT_GT(cs.depth_range[0], 100.0F);
}

// Pancaked (the default): each cascade's depth spans only its bounding
// sphere — far finer depth steps — and casters toward the sun fall before
// depth 0, where the shadow pipeline's depth clamp pins them.
TEST(CsmFit, PancakedRangeIsTightAndClampsCasters) {
    const r::CsmView v = make_view(glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F));
    r::CsmSettings s;
    s.caster_distance = 100.0F;
    const r::GpuCascades tight = r::fit_cascades(v, s, glm::vec2(0.1F, 60.0F), k_map);
    const r::GpuCascades padded =
        r::fit_cascades(v, s, glm::vec2(0.1F, 60.0F), k_map, /*pancaked=*/false);
    for (int c = 0; c < 4; ++c) {
        // Depth spans the sphere's diameter: 2 * texel * map / 2.
        EXPECT_NEAR(tight.depth_range[c], tight.texel_world[c] * static_cast<float>(k_map),
                    1e-3F * tight.depth_range[c]);
        EXPECT_NEAR(padded.depth_range[c] - tight.depth_range[c], 100.0F, 1e-2F);
        for (int col = 0; col < 4; ++col) { // same x / y mapping (rows 0 and 1)
            const auto cs = static_cast<std::size_t>(c);
            EXPECT_EQ(tight.view_proj[cs][col][0], padded.view_proj[cs][col][0]);
            EXPECT_EQ(tight.view_proj[cs][col][1], padded.view_proj[cs][col][1]);
        }
    }
    EXPECT_EQ(tight.limits.y, 100.0F);
    const glm::vec3 receiver(0.0F, 0.0F, -2.0F);
    const glm::vec4 rc = tight.view_proj[0] * glm::vec4(receiver, 1.0F);
    const glm::vec4 cc = tight.view_proj[0] * glm::vec4(receiver + v.to_light * 80.0F, 1.0F);
    EXPECT_GE(rc.z, 0.0F);
    EXPECT_LE(rc.z, 1.0F);
    EXPECT_LT(cc.z, 0.0F); // clamped to 0 by the rasteriser
    EXPECT_NEAR(cc.x, rc.x, 1e-4F);
}

// Rotating the camera in place keeps every cascade's size (texel world size),
// and a fixed world point lands on texel positions that differ by whole
// texels (the snapping grid does not move with the camera).
TEST(CsmStability, RotationKeepsTexelSizeAndGrid) {
    r::CsmSettings s;
    const glm::vec3 eye(5.0F, 3.0F, 5.0F);
    const glm::vec3 probe(7.3F, 0.0F, 1.9F);
    const r::GpuCascades a =
        r::fit_cascades(make_view(eye, glm::vec3(0.0F, -0.2F, -1.0F)), s, {0.1F, 100.0F}, k_map);
    for (float yaw : {0.3F, 1.1F, 2.6F}) {
        const glm::vec3 fwd(std::sin(yaw), -0.2F, -std::cos(yaw));
        const r::GpuCascades b = r::fit_cascades(make_view(eye, fwd), s, {0.1F, 100.0F}, k_map);
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(a.texel_world[c], b.texel_world[c]) << "yaw " << yaw << " cascade " << c;
            const glm::vec2 d = texel_of(b, c, probe) - texel_of(a, c, probe);
            EXPECT_NEAR(d.x, std::round(d.x), 2e-3F) << "yaw " << yaw << " cascade " << c;
            EXPECT_NEAR(d.y, std::round(d.y), 2e-3F) << "yaw " << yaw << " cascade " << c;
        }
    }
}

// Translating the camera shifts each cascade by whole texels only.
TEST(CsmStability, TranslationMovesByWholeTexels) {
    r::CsmSettings s;
    const glm::vec3 fwd(0.2F, -0.3F, -1.0F);
    const glm::vec3 probe(1.0F, 0.5F, -6.0F);
    const r::GpuCascades a =
        r::fit_cascades(make_view(glm::vec3(0.0F, 2.0F, 0.0F), fwd), s, {0.1F, 100.0F}, k_map);
    for (float step : {0.013F, 0.37F, 1.9F}) {
        const glm::vec3 eye(step, 2.0F + step * 0.25F, -step * 0.5F);
        const r::GpuCascades b = r::fit_cascades(make_view(eye, fwd), s, {0.1F, 100.0F}, k_map);
        for (int c = 0; c < 4; ++c) {
            ASSERT_EQ(a.texel_world[c], b.texel_world[c]);
            const glm::vec2 d = texel_of(b, c, probe) - texel_of(a, c, probe);
            EXPECT_NEAR(d.x, std::round(d.x), 2e-3F) << "step " << step << " cascade " << c;
            EXPECT_NEAR(d.y, std::round(d.y), 2e-3F) << "step " << step << " cascade " << c;
        }
    }
}

TEST(CsmCulling, CasterVolumeHoldsEveryCascade) {
    const r::CsmView v = make_view(glm::vec3(0.0F, 3.0F, 0.0F), glm::vec3(0.3F, -0.2F, -1.0F));
    r::CsmSettings s;
    const glm::mat4 cull = r::caster_cull_view_proj(v, s, k_map);
    const glm::vec2 range = r::shadow_range(v.projection, s, false, 0.0F, 0.0F).range;
    for (const glm::vec3& p : slice_corners(v, range.x, range.y)) {
        for (float lift : {0.0F, 50.0F, 150.0F}) {
            const glm::vec4 clip = cull * glm::vec4(p + v.to_light * lift, 1.0F);
            EXPECT_LE(std::abs(clip.x), 1.0001F);
            EXPECT_LE(std::abs(clip.y), 1.0001F);
            EXPECT_GE(clip.z, 0.0F);
            EXPECT_LE(clip.z, 1.0F);
        }
    }
}

// Depth bounds jittering across a quantisation boundary (TAA) do not flip the
// fitted range between steps; the range grows at once (it always covers the
// raw bounds) and shrinks only once the raw value is a quarter step past.
TEST(CsmRange, HysteresisKeepsTheStepUnderJitter) {
    r::CsmRangeState state;
    // Far bound oscillating around 32 (steps: 2 below 32, 4 above).
    const float his[] = {31.9F, 32.1F, 31.8F, 32.2F, 31.95F, 32.05F};
    std::vector<float> kept;
    for (float raw_hi : his) {
        const glm::vec2 q = r::stabilized_range(10.0F, raw_hi, state);
        EXPECT_GE(q.y, raw_hi);
        EXPECT_LE(q.x, 10.0F);
        state = {q.x, q.y, true};
        kept.push_back(q.y);
    }
    EXPECT_FLOAT_EQ(kept[0], 32.0F);
    for (std::size_t i = 1; i < kept.size(); ++i) EXPECT_FLOAT_EQ(kept[i], 36.0F) << i;

    // Moving inward by more than a quarter step below the kept step shrinks it.
    const glm::vec2 shrunk = r::stabilized_range(10.0F, 30.5F, state);
    EXPECT_FLOAT_EQ(shrunk.y, 32.0F);
    // A small inward move does not.
    EXPECT_FLOAT_EQ(r::stabilized_range(10.0F, 31.5F, state).y, 36.0F);

    // Near bound: same rules mirrored (grows outward at once, inward lazily).
    r::CsmRangeState near_state{10.0F, 40.0F, true};
    EXPECT_FLOAT_EQ(r::stabilized_range(9.9F, 39.0F, near_state).x, 9.0F); // closer: at once
    EXPECT_FLOAT_EQ(r::stabilized_range(10.9F, 39.0F, near_state).x, 10.0F); // kept
    EXPECT_FLOAT_EQ(r::stabilized_range(11.4F, 39.0F, near_state).x, 11.0F); // > 1.25 steps
    // Without history the plain quantisation applies.
    EXPECT_FLOAT_EQ(r::stabilized_range(10.9F, 31.9F, {}).x, 10.0F);
    EXPECT_FLOAT_EQ(r::stabilized_range(10.9F, 31.9F, {}).y, 32.0F);
}

TEST(CsmRange, FittedRangeReportsTheShadowLimit) {
    const glm::mat4 proj = projection();
    r::CsmSettings s;
    s.max_distance = 150.0F;
    const auto depth_at = [&](float dist) {
        const glm::vec4 c = proj * glm::vec4(0.0F, 0.0F, -dist, 1.0F);
        return c.z / c.w;
    };
    const r::ShadowRange fit = r::shadow_range(proj, s, true, depth_at(5.0F), depth_at(19.5F));
    EXPECT_TRUE(fit.fitted);
    EXPECT_NEAR(fit.full_far, 150.0F, 1e-2F);
    EXPECT_LT(fit.range.y, fit.full_far);
    EXPECT_TRUE(fit.state.valid);
    const r::GpuCascades cs = r::fit_cascades(make_view(glm::vec3(0.0F), glm::vec3(0, 0, -1)), s,
                                              fit.range, k_map);
    EXPECT_NEAR(cs.limits.x, 150.0F, 1e-2F);
}
