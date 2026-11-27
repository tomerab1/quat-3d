// Camera framing (issue #8): frame_box puts every corner of the box inside
// the view (with the margin), and is tight — some corner touches the shrunk
// frustum — so the content fills the frame.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/scene/camera_framing.hpp"

namespace {

using engine::scene::FramingParams;

// Largest |ndc| over the box corners seen from `eye` along the framing
// direction (1 = the frustum edge), and whether all are in front of `near`.
struct Fit {
    float max_ndc = 0.0F;
    bool  in_front = true;
};

Fit fit(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& eye, const FramingParams& p) {
    const glm::vec3 fwd = engine::scene::framing_forward(p.yaw, p.pitch);
    const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0.0F, 1.0F, 0.0F));
    const float ty = std::tan(0.5F * p.fov_y);
    const float tx = ty * p.aspect;
    Fit out;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec3 v(view * glm::vec4(c, 1.0F));
        const float depth = -v.z;
        if (depth < p.near_z - 1e-4F) out.in_front = false;
        out.max_ndc = std::max({out.max_ndc, std::abs(v.x) / (tx * depth),
                                std::abs(v.y) / (ty * depth)});
    }
    return out;
}

TEST(CameraFraming, WideFlatContentFillsTheFrameWithTheMargin) {
    // The showcase content: two rows of spheres, a glow cube, a glass sphere.
    const glm::vec3 lo(-6.2F, 0.0F, -2.2F);
    const glm::vec3 hi(4.3F, 2.1F, 5.1F);
    FramingParams p;
    p.yaw = std::atan2(-1.0F, -0.5F);
    p.pitch = glm::radians(-25.0F);
    p.fov_y = glm::radians(60.0F);
    p.aspect = 16.0F / 9.0F;
    p.margin = 0.1F;

    const glm::vec3 eye = engine::scene::frame_box(lo, hi, p);
    const Fit f = fit(lo, hi, eye, p);
    EXPECT_TRUE(f.in_front);
    EXPECT_LE(f.max_ndc, 0.9F + 1e-4F) << "a corner falls in the margin or outside";
    EXPECT_NEAR(f.max_ndc, 0.9F, 1e-3F) << "framing is not tight";
    EXPECT_GT(eye.y, hi.y) << "looking down at 25 degrees from above the content";
}

TEST(CameraFraming, DeterministicAndAspectAware) {
    const glm::vec3 lo(-1.0F);
    const glm::vec3 hi(1.0F);
    FramingParams p;
    p.yaw = 0.3F;
    p.pitch = -0.4F;
    p.fov_y = glm::radians(50.0F);
    p.aspect = 2.0F;
    EXPECT_EQ(engine::scene::frame_box(lo, hi, p), engine::scene::frame_box(lo, hi, p));
    const float wide = glm::length(engine::scene::frame_box(lo, hi, p));
    p.aspect = 0.5F; // a narrow view has to back off further
    const float narrow = glm::length(engine::scene::frame_box(lo, hi, p));
    EXPECT_GT(narrow, wide);
    EXPECT_LE(fit(lo, hi, engine::scene::frame_box(lo, hi, p), p).max_ndc, 0.9F + 1e-4F);
}

TEST(CameraFraming, TinyBoxStaysBeyondTheNearPlane) {
    FramingParams p;
    p.near_z = 0.5F;
    const glm::vec3 eye = engine::scene::frame_box(glm::vec3(0.0F), glm::vec3(0.0F), p);
    EXPECT_NEAR(glm::length(eye), 0.5F, 1e-5F);
}

} // namespace
