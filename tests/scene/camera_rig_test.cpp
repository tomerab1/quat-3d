// Camera rig (G15.1): spring convergence, pitch easing, zoom-to-cursor
// maths, grab-the-world drag, bounds, terrain clearance, framing, and the
// system driven by G5 actions.

#include <cmath>
#include <optional>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/core/spring.hpp"
#include "engine/input/actions.hpp"
#include "engine/input/device_state.hpp"
#include "engine/scene/camera_rig.hpp"
#include "engine/scene/components.hpp"

namespace quat::test {
namespace {

namespace s = engine::scene;
using engine::core::spring_damper;

constexpr glm::vec2 k_viewport{1280.0F, 720.0F};
constexpr float k_fov = 1.0471976F;

[[nodiscard]] s::RigView view_of(const s::CameraRig& rig) {
    return {rig.focus, rig.yaw_deg, s::rig_pitch_for_distance(rig, rig.distance), rig.distance, k_fov,
            k_viewport.x / k_viewport.y};
}

TEST(CameraRigSpring, HalfLifeNoOvershootAndFrameRateIndependence) {
    // From rest, after one half-life the exponential envelope has halved
    // twice (y h = 2 ln 2): x = 1 - (1 + 2 ln 2) / 4 of the way.
    float x = 0.0F;
    float v = 0.0F;
    spring_damper(x, v, 1.0F, 0.2F, 0.2F);
    EXPECT_NEAR(x, 1.0F - (1.0F + 2.0F * std::log(2.0F)) / 4.0F, 1e-5F);

    // Never overshoots a fixed goal, converges, and the result does not
    // depend on how the time is sliced.
    float a = 0.0F, va = 0.0F, b = 0.0F, vb = 0.0F;
    float worst = 0.0F;
    for (int i = 0; i < 240; ++i) {
        spring_damper(a, va, 10.0F, 0.15F, 1.0F / 60.0F);
        worst = std::max(worst, a);
    }
    for (int i = 0; i < 960; ++i) spring_damper(b, vb, 10.0F, 0.15F, 1.0F / 240.0F);
    EXPECT_LE(worst, 10.0F + 1e-4F);
    EXPECT_NEAR(a, 10.0F, 1e-3F);
    EXPECT_NEAR(a, b, 1e-4F);

    // Frame-rate independence where it matters: mid-transition, from a
    // moving start (review #111) — 6 x 1/60 s against 24 x 1/240 s.
    float p = 0.0F, vp = 25.0F, q = 0.0F, vq = 25.0F;
    for (int i = 0; i < 6; ++i) spring_damper(p, vp, 10.0F, 0.15F, 1.0F / 60.0F);
    for (int i = 0; i < 24; ++i) spring_damper(q, vq, 10.0F, 0.15F, 1.0F / 240.0F);
    ASSERT_GT(std::abs(p - 10.0F), 1.0F) << "still mid-transition";
    EXPECT_NEAR(p, q, 1e-4F) << "frame-rate independent";
    EXPECT_NEAR(vp, vq, 1e-3F);

    // Zero half-life snaps.
    float c = 3.0F, vc = 5.0F;
    spring_damper(c, vc, -1.0F, 0.0F, 0.016F);
    EXPECT_EQ(c, -1.0F);
    EXPECT_EQ(vc, 0.0F);

    // Angles take the short way round.
    float yaw = 170.0F, vy = 0.0F;
    for (int i = 0; i < 120; ++i) engine::core::spring_damper_degrees(yaw, vy, -170.0F, 0.1F, 1.0F / 60.0F);
    EXPECT_NEAR(engine::core::wrap_degrees(yaw), -170.0F, 1e-2F);
    EXPECT_GT(yaw, 170.0F) << "went through 180, not back through 0";
}

TEST(CameraRigMaths, PitchEasesWithLogDistance) {
    s::CameraRig rig;
    EXPECT_FLOAT_EQ(s::rig_pitch_for_distance(rig, rig.min_distance), rig.pitch_near_deg);
    EXPECT_FLOAT_EQ(s::rig_pitch_for_distance(rig, rig.max_distance), rig.pitch_far_deg);
    const float mid = std::sqrt(rig.min_distance * rig.max_distance); // geometric mean
    EXPECT_NEAR(s::rig_pitch_for_distance(rig, mid), 0.5F * (rig.pitch_near_deg + rig.pitch_far_deg), 1e-3F);
    EXPECT_FLOAT_EQ(s::rig_pitch_for_distance(rig, 1.0e6F), rig.pitch_far_deg) << "clamped";
    rig.pitch_follows_zoom = false;
    rig.pitch_deg = -30.0F;
    EXPECT_FLOAT_EQ(s::rig_pitch_for_distance(rig, mid), -30.0F);
}

TEST(CameraRigMaths, WorldMatrixLooksAtTheFocus) {
    const glm::vec3 focus(3.0F, 1.0F, -2.0F);
    for (float pitch : {-10.0F, -60.0F, -89.0F}) {
        const glm::vec3 eye = s::rig_eye(focus, 30.0F, pitch, 25.0F);
        const glm::mat4 m = s::rig_world_matrix(eye, 30.0F, pitch);
        const glm::vec3 look = -glm::vec3(m[2]);
        EXPECT_NEAR(glm::length(eye + look * 25.0F - focus), 0.0F, 1e-4F) << pitch;
        EXPECT_GT(m[1][1], 0.0F) << "up stays up";
        EXPECT_NEAR(glm::determinant(glm::mat3(m)), 1.0F, 1e-5F) << "a rotation";
    }
}

TEST(CameraRigMaths, ZoomToCursorKeepsTheGroundPointUnderTheCursor) {
    s::CameraRig rig;
    rig.focus = glm::vec3(10.0F, 0.0F, 5.0F);
    rig.distance = 60.0F;
    const glm::vec2 cursor(1000.0F, 200.0F);
    const s::RigView before = view_of(rig);
    const auto ground = s::ray_plane_y(s::rig_pointer_ray(before, cursor, k_viewport), 0.0F);
    ASSERT_TRUE(ground.has_value());
    for (float steps : {3.0F, -2.0F, 0.5F}) {
        const s::ZoomResult z = s::rig_zoom_to_cursor(rig, before, steps, 1.15F, cursor, k_viewport, *ground);
        EXPECT_NEAR(z.distance, std::clamp(60.0F * std::pow(1.15F, -steps), rig.min_distance, rig.max_distance), 1e-3F);
        EXPECT_FLOAT_EQ(z.pitch_deg, s::rig_pitch_for_distance(rig, z.distance)) << "pitch eases with the zoom";
        const s::RigView after{z.focus, rig.yaw_deg, z.pitch_deg, z.distance, k_fov, before.aspect};
        const auto again = s::ray_plane_y(s::rig_pointer_ray(after, cursor, k_viewport), 0.0F);
        ASSERT_TRUE(again.has_value());
        EXPECT_NEAR(glm::length(*again - *ground), 0.0F, 2e-3F) << "steps " << steps;
    }
    // At the centre of the screen zooming moves straight along the view axis.
    const s::ZoomResult centre = s::rig_zoom_to_cursor(rig, before, 1.0F, 1.15F, k_viewport * 0.5F,
                                                       k_viewport, rig.focus);
    EXPECT_NEAR(glm::length(centre.focus - rig.focus), 0.0F, 1e-3F);
}

TEST(CameraRigMaths, Bounds) {
    s::RigBounds b;
    b.shape = s::RigBounds::Shape::rect;
    b.min = {-10.0F, -20.0F};
    b.max = {10.0F, 20.0F};
    b.margin = 5.0F;
    EXPECT_EQ(s::rig_clamp_hard(b, {50.0F, 3.0F, -30.0F}), glm::vec3(10.0F, 3.0F, -20.0F));
    EXPECT_EQ(s::rig_clamp_hard(b, {1.0F, 0.0F, 2.0F}), glm::vec3(1.0F, 0.0F, 2.0F));
    // Soft: unchanged inside, monotone outside, never past the margin.
    float last = 10.0F;
    for (float x = 10.0F; x < 1000.0F; x *= 1.3F) {
        const float soft = s::rig_clamp_soft(b, {x, 0.0F, 0.0F}).x;
        EXPECT_GE(soft, last - 1e-5F);
        EXPECT_LE(soft, 15.0F) << "never past the margin";
        last = soft;
    }
    EXPECT_GT(last, 14.9F);
    EXPECT_NEAR(s::rig_clamp_soft(b, {10.01F, 0.0F, 0.0F}).x, 10.01F, 1e-4F) << "slope 1 at the bound";

    s::RigBounds c;
    c.shape = s::RigBounds::Shape::circle;
    c.center = {5.0F, 5.0F};
    c.radius = 10.0F;
    c.margin = 2.0F;
    const glm::vec3 far(105.0F, 0.0F, 5.0F);
    EXPECT_NEAR(s::rig_clamp_hard(c, far).x, 15.0F, 1e-4F);
    EXPECT_LE(s::rig_clamp_soft(c, far).x, 17.0F);
    EXPECT_GT(s::rig_clamp_soft(c, far).x, 15.0F);
}

TEST(CameraRigMaths, TerrainClearanceLooksDownInsteadOfMovingTheFocus) {
    // A ridge 30 m high behind the focus (the camera sits over it at the
    // default pitch).
    const s::RigGroundFn ground = [](float x, float) -> std::optional<float> {
        return x < -15.0F ? 30.0F : 0.0F;
    };
    const glm::vec3 focus(0.0F);
    const float yaw = 0.0F; // looking along +X: the eye is at -X
    const float pitch = -20.0F;
    const float distance = 60.0F;
    ASSERT_LT(s::rig_eye(focus, yaw, pitch, distance).y, 32.0F) << "the setup must start below the ridge";
    const float clear = s::rig_clear_pitch(ground, focus, yaw, pitch, distance, 2.0F, -89.0F);
    EXPECT_LT(clear, pitch);
    const glm::vec3 eye = s::rig_eye(focus, yaw, clear, distance);
    EXPECT_GE(eye.y, *ground(eye.x, eye.z) + 2.0F - 1e-3F);
    // Already clear: unchanged.
    EXPECT_FLOAT_EQ(s::rig_clear_pitch(ground, focus, yaw, -80.0F, distance, 2.0F, -89.0F), -80.0F);
}

TEST(CameraRigMaths, FrameBoxFitsAtTheCurrentYaw) {
    s::CameraRig rig;
    rig.yaw_deg = 30.0F;
    s::camera_rig_frame(rig, glm::vec3(-20.0F, 0.0F, -5.0F), glm::vec3(20.0F, 8.0F, 5.0F), k_fov,
                        k_viewport.x / k_viewport.y);
    EXPECT_EQ(rig.focus, glm::vec3(0.0F, 4.0F, 0.0F));
    EXPECT_GT(rig.distance, 20.0F);
    EXPECT_LT(rig.distance, 120.0F);
    EXPECT_FLOAT_EQ(rig.yaw_deg, 30.0F) << "framing keeps the heading";
}

// ---- The system --------------------------------------------------------------

struct RigWorld {
    entt::registry r;
    entt::entity   e = r.create();
    engine::input::Actions actions;
    engine::input::DeviceState device;
    s::RigEnvironment env;

    void init() {
        auto map = engine::input::parse_action_map(nlohmann::json::parse(R"({
            "actions": [
              {"name": "pan", "type": "axis2d", "bindings": [{"composite": "2d", "up": "key:w", "down": "key:s", "left": "key:a", "right": "key:d"}]},
              {"name": "orbit", "type": "axis1d", "bindings": [{"composite": "1d", "negative": "key:q", "positive": "key:e"}]},
              {"name": "zoom", "type": "axis1d", "bindings": ["mouse:wheel"]},
              {"name": "drag", "type": "button", "trigger": {"drag": 3}, "bindings": ["mouse:middle"]}]})"));
        ASSERT_TRUE(map.has_value());
        actions.set_map(*map);
        r.emplace<s::Transform>(e);
        r.emplace<s::Camera>(e);
        auto& rig = r.emplace<s::CameraRig>(e);
        rig.focus = glm::vec3(0.0F);
        rig.yaw_deg = 0.0F;
        rig.distance = 40.0F;
        auto& in = r.emplace<s::CameraRigInput>(e);
        in.pan = "pan";
        in.orbit = "orbit";
        in.zoom = "zoom";
        in.drag_pan = "drag";
        env.actions = &actions;
        env.viewport_px = k_viewport;
        env.pointer = k_viewport * 0.5F;
        device.viewport_size = k_viewport;
        device.mouse_position = env.pointer;
    }
    template <typename F>
    void frame(F&& edit, float dt = 1.0F / 60.0F) {
        device.begin_frame();
        edit(device);
        actions.update(device, dt);
        env.pointer = device.pointer();
        s::camera_rig_system(r, dt, env);
    }
    void settle(int frames = 240) {
        for (int i = 0; i < frames; ++i) frame([](engine::input::DeviceState&) {});
    }
    s::CameraRig& rig() { return r.get<s::CameraRig>(e); }
};

struct RigFixture : ::testing::Test, RigWorld {
    void SetUp() override { init(); }
};

TEST_F(RigFixture, WritesTheTransformAndSettlesOnTheTargets) {
    settle(1);
    const glm::mat4& m = r.get<s::Transform>(e).local;
    const glm::vec3 eye(m[3]);
    EXPECT_NEAR(glm::length(eye - s::rig_eye(glm::vec3(0.0F), 0.0F, s::rig_pitch_for_distance(rig(), 40.0F), 40.0F)),
                0.0F, 1e-3F)
        << "first update snaps to the targets";
    rig().focus = glm::vec3(30.0F, 0.0F, 0.0F);
    rig().yaw_deg = 90.0F;
    rig().distance = 100.0F;
    settle();
    EXPECT_NEAR(rig().state.focus.x, 30.0F, 1e-2F);
    EXPECT_NEAR(rig().state.yaw_deg, 90.0F, 1e-2F);
    EXPECT_NEAR(std::exp(rig().state.log_distance), 100.0F, 1e-1F);
}

TEST_F(RigFixture, PansInTheYawFrameProportionalToDistance) {
    rig().yaw_deg = 90.0F; // forward is +Z
    settle(1);
    frame([](engine::input::DeviceState& d) { d.keys.set(*engine::input::key_from_name("w")); }, 0.5F);
    EXPECT_NEAR(rig().focus.z, 0.5F * 40.0F, 1e-3F) << "pan_speed 1: one distance per second";
    EXPECT_NEAR(rig().focus.x, 0.0F, 1e-4F);
}

TEST_F(RigFixture, OrbitStepsWhenYawStepIsSet) {
    rig().yaw_step_deg = 45.0F;
    settle(1);
    frame([](engine::input::DeviceState& d) { d.keys.set(*engine::input::key_from_name("e")); });
    EXPECT_FLOAT_EQ(rig().yaw_deg, 45.0F);
    frame([](engine::input::DeviceState&) {}); // still held: no repeat
    EXPECT_FLOAT_EQ(rig().yaw_deg, 45.0F);
}

TEST_F(RigFixture, WheelZoomsTowardTheCursor) {
    settle(1);
    device.mouse_position = {1100.0F, 150.0F};
    env.pointer = device.mouse_position;
    const s::RigView before{rig().state.focus, rig().state.yaw_deg, rig().state.pitch_deg,
                            std::exp(rig().state.log_distance), k_fov, k_viewport.x / k_viewport.y};
    const auto ground = s::ray_plane_y(s::rig_pointer_ray(before, env.pointer, k_viewport), 0.0F);
    ASSERT_TRUE(ground.has_value());
    frame([](engine::input::DeviceState& d) { d.mouse_wheel = 2.0F; });
    settle();
    const s::RigView after{rig().state.focus, rig().state.yaw_deg, rig().state.pitch_deg,
                           std::exp(rig().state.log_distance), k_fov, k_viewport.x / k_viewport.y};
    EXPECT_LT(after.distance, before.distance);
    const auto again = s::ray_plane_y(s::rig_pointer_ray(after, env.pointer, k_viewport), 0.0F);
    ASSERT_TRUE(again.has_value());
    EXPECT_NEAR(glm::length(*again - *ground), 0.0F, 0.05F) << "the point under the cursor stayed";
}

TEST_F(RigFixture, DragGrabsTheWorldThenSpringsBackIntoBounds) {
    rig().bounds.shape = s::RigBounds::Shape::rect;
    rig().bounds.min = {-5.0F, -5.0F};
    rig().bounds.max = {5.0F, 5.0F};
    rig().bounds.margin = 4.0F;
    settle(1);
    const glm::vec2 start(640.0F, 500.0F);
    const s::RigView v0{rig().state.focus, rig().state.yaw_deg, rig().state.pitch_deg,
                        std::exp(rig().state.log_distance), k_fov, k_viewport.x / k_viewport.y};
    const glm::vec3 grabbed = *s::ray_plane_y(s::rig_pointer_ray(v0, start, k_viewport), 0.0F);
    frame([&](engine::input::DeviceState& d) {
        d.mouse.set(1);
        d.mouse_position = start;
    });
    // Small moves: inside the bounds the grabbed point follows the pointer 1:1.
    for (int i = 1; i <= 10; ++i) {
        frame([&](engine::input::DeviceState& d) { d.mouse_position = start + glm::vec2(0.0F, -2.0F * static_cast<float>(i)); });
    }
    ASSERT_TRUE(rig().state.dragging);
    const s::RigView v1{rig().state.focus, rig().state.yaw_deg, rig().state.pitch_deg,
                        std::exp(rig().state.log_distance), k_fov, k_viewport.x / k_viewport.y};
    const auto under = s::ray_plane_y(s::rig_pointer_ray(v1, device.pointer(), k_viewport), 0.0F);
    ASSERT_TRUE(under.has_value());
    EXPECT_NEAR(glm::length(*under - grabbed), 0.0F, 1e-2F) << "grab the world: 1:1";
    // Drag far out: the rubber band holds the focus within the margin.
    for (int i = 1; i <= 30; ++i) {
        frame([&](engine::input::DeviceState& d) { d.mouse_position.y -= 20.0F; });
    }
    EXPECT_LT(std::abs(rig().state.focus.x), 5.0F + 4.0F + 1e-3F);
    EXPECT_LT(std::abs(rig().state.focus.z), 5.0F + 4.0F + 1e-3F);
    // Release: it springs back inside.
    frame([](engine::input::DeviceState& d) { d.mouse.reset(1); });
    EXPECT_FALSE(rig().state.dragging);
    settle(300);
    EXPECT_LE(std::abs(rig().state.focus.x), 5.0F + 1e-3F);
    EXPECT_LE(std::abs(rig().state.focus.z), 5.0F + 1e-3F);
}

// Review #111: the release glide is the exact integral of the decaying
// velocity, so a flick travels the same distance at 30, 60, 144 and 240 Hz.
TEST(CameraRigSystem, InertiaGlideIsFrameRateIndependent) {
    const auto glide = [](int hz) {
        entt::registry r;
        const entt::entity e = r.create();
        r.emplace<s::Transform>(e);
        auto& rig = r.emplace<s::CameraRig>(e);
        rig.focus_halflife_s = 0.0F; // read the target directly
        s::camera_rig_system(r, 0.0F, {});
        rig.state.drag_vel = glm::vec2(10.0F, 0.0F);
        const float dt = 1.0F / static_cast<float>(hz);
        for (int i = 0; i < hz * 3; ++i) s::camera_rig_system(r, dt, {});
        return rig.focus.x;
    };
    const float expected = 10.0F * 0.15F / std::log(2.0F); // v0 * h / ln 2
    for (const int hz : {30, 60, 144, 240}) EXPECT_NEAR(glide(hz), expected, 0.01F) << hz << " Hz";
}

TEST_F(RigFixture, YawStepsStayOnTheLatticeAcrossTheWrap) {
    rig().yaw_step_deg = 90.0F;
    rig().yaw_deg = 90.0F;
    settle(1);
    for (int i = 0; i < 3; ++i) {
        frame([](engine::input::DeviceState& d) { d.keys.set(*engine::input::key_from_name("e")); });
        frame([](engine::input::DeviceState& d) { d.keys.reset(); });
    }
    EXPECT_FLOAT_EQ(rig().yaw_deg, 0.0F) << "90 + 3 x 90 wraps to 0";
}

TEST_F(RigFixture, FollowsTheGroundAndKeepsClearance) {
    env.ground = [](float x, float) -> std::optional<float> { return x < -20.0F ? 40.0F : 3.0F; };
    rig().yaw_deg = 0.0F;      // the eye is toward -X, over the plateau
    rig().pitch_follows_zoom = false;
    rig().pitch_deg = -20.0F;
    rig().distance = 80.0F;
    settle();
    EXPECT_NEAR(rig().state.focus.y, 3.0F, 1e-3F) << "focus on the ground";
    const glm::vec3 eye(r.get<s::Transform>(e).local[3]);
    EXPECT_GE(eye.y, 40.0F + rig().ground_clearance - 1e-2F);
    EXPECT_LT(rig().state.pitch_deg, -20.0F) << "looked down instead";
}

TEST_F(RigFixture, Deterministic) {
    const auto run = [] {
        RigWorld f;
        f.init();
        for (int i = 0; i < 200; ++i) {
            f.frame([i](engine::input::DeviceState& d) {
                d.keys.set(*engine::input::key_from_name("d"), (i / 20) % 2 == 0);
                d.keys.set(*engine::input::key_from_name("q"), (i / 30) % 2 == 1);
                d.mouse_wheel = i % 17 == 0 ? 1.0F : 0.0F;
                d.mouse_position = {600.0F + static_cast<float>(i), 300.0F};
            }, 1.0F / (45.0F + static_cast<float>(i % 7)));
        }
        return f.r.get<s::Transform>(f.e).local;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
} // namespace quat::test
