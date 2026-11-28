// The physics demo (QUAT_PHYSICS=1) stepped through the frame loop's own
// simulation path — WorldRuntime::build_scene, then WorldRuntime::simulate
// with the SimClock's dt each frame, exactly as Application does — must
// leave every box resting on the floor (issue #21: the boxes were created
// at the origin inside the floor and fell out of the world) and outside the
// glass panel (issue #46: the panel had no collider).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "app/world_runtime.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/sim_clock.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/gpu_context.hpp"

namespace {

constexpr float k_real_dt = 1.0F / 60.0F;
constexpr std::size_t k_boxes = 12; // build_demo_scene's pile
constexpr float k_floor_top = 0.0F;
constexpr float k_box_half = 0.5F;

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        ::setenv(name, value, 1);
    } else {
        ::unsetenv(name);
    }
#endif
}

// Sets an environment variable for one scope (the demo scene reads it).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
            had_ = true;
        }
        set_env(name, value);
    }
    ~ScopedEnv() { set_env(name_, had_ ? old_.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::string old_;
    bool        had_ = false;
};

struct DemoResult {
    std::vector<glm::mat4> boxes; // world transforms of the dynamic boxes
    // The glass panel as drawn (its mesh bounds in world space), so the
    // overlap check does not depend on the collider under test.
    glm::vec3 panel_centre{0.0F};
    glm::vec3 panel_half{0.0F};
    bool      has_panel = false;
    bool      panel_has_collider = false;
};

// Runs the physics demo for `frames` frames at `time_scale`.
DemoResult run_physics_demo(quat::test::GpuContext& ctx, int frames, float time_scale) {
    engine::scene::Scene scene;
    engine::asset::AssetManager assets;
    quat::app::WorldRuntime world;
    {
        const ScopedEnv physics("QUAT_PHYSICS", "1");
        engine::renderer::RenderSettings settings;
        EXPECT_TRUE(world.build_scene(scene, assets, ctx.allocator, ctx.transfer,
                                      QUAT_PROJECT_ROOT, settings));
    }
    EXPECT_TRUE(world.physics_mode());

    engine::core::SimClock clock;
    clock.set_time_scale(time_scale);
    for (int frame = 0; frame < frames; ++frame) {
        world.simulate(scene, static_cast<std::uint64_t>(frame), k_real_dt,
                       clock.advance(k_real_dt), nullptr);
    }

    DemoResult out;
    auto& r = scene.registry();
    for (auto [e, name, t] : r.view<engine::scene::Name, engine::scene::Transform>().each()) {
        if (name.value == "box") out.boxes.push_back(t.world);
        const auto* mr = r.try_get<engine::scene::MeshRenderer>(e);
        if (name.value == "glass" && mr != nullptr && mr->mesh.is_loaded()) {
            const engine::asset::Aabb& b = mr->mesh->bounds; // unrotated, unscaled panel
            out.panel_centre = glm::vec3(t.world * glm::vec4(0.5F * (b.min + b.max), 1.0F));
            out.panel_half = 0.5F * (b.max - b.min);
            out.has_panel = true;
            out.panel_has_collider = r.all_of<engine::scene::Collider, engine::scene::RigidBody>(e);
        }
    }
    return out;
}

void expect_boxes_rest_on_the_floor(const DemoResult& demo) {
    ASSERT_EQ(demo.boxes.size(), k_boxes);
    for (std::size_t i = 0; i < demo.boxes.size(); ++i) {
        const float y = demo.boxes[i][3].y;
        // A box centre is at least half a box above the floor when resting
        // on it; the pile is at most a few boxes high.
        EXPECT_GE(y, k_floor_top + 0.4F) << "box " << i << " fell through the floor";
        EXPECT_LE(y, k_floor_top + 15.0F) << "box " << i << " never landed";
    }
}

// Separating-axis test of an oriented box (world transform `box`, half
// extents `bh`) against an axis-aligned box (centre `ac`, half extents `ah`),
// both shrunk by `slop` (resting contacts interpenetrate a little).
bool boxes_overlap(const glm::mat4& box, const glm::vec3& bh, const glm::vec3& ac,
                   const glm::vec3& ah, float slop) {
    const glm::vec3 bu[3] = {glm::normalize(glm::vec3(box[0])), glm::normalize(glm::vec3(box[1])),
                             glm::normalize(glm::vec3(box[2]))};
    const glm::vec3 au[3] = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    const glm::vec3 b = bh - glm::vec3(slop);
    const glm::vec3 a = ah - glm::vec3(slop);
    const glm::vec3 d = glm::vec3(box[3]) - ac;
    std::vector<glm::vec3> axes{au[0], au[1], au[2], bu[0], bu[1], bu[2]};
    for (const glm::vec3& x : au) {
        for (const glm::vec3& y : bu) {
            const glm::vec3 c = glm::cross(x, y);
            if (glm::dot(c, c) > 1e-8F) axes.push_back(glm::normalize(c));
        }
    }
    for (const glm::vec3& n : axes) {
        float ra = 0.0F;
        float rb = 0.0F;
        for (int k = 0; k < 3; ++k) {
            ra += a[k] * std::abs(glm::dot(au[k], n));
            rb += b[k] * std::abs(glm::dot(bu[k], n));
        }
        if (std::abs(glm::dot(d, n)) > ra + rb) return false; // a separating axis
    }
    return true;
}

void expect_no_box_in_the_panel(const DemoResult& demo) {
    ASSERT_TRUE(demo.has_panel) << "no glass panel in the physics demo";
    EXPECT_TRUE(demo.panel_has_collider) << "the glass panel has no collider";
    for (std::size_t i = 0; i < demo.boxes.size(); ++i) {
        EXPECT_FALSE(boxes_overlap(demo.boxes[i], glm::vec3(k_box_half), demo.panel_centre,
                                   demo.panel_half, 0.03F))
            << "box " << i << " is inside the glass panel";
    }
}

TEST(GpuDemoPhysics, BoxesComeToRestOnTheFloor) {
    QUAT_REQUIRE_GPU(ctx);
    // 4 simulated seconds: the highest box starts 15.65 m up.
    const DemoResult demo = run_physics_demo(*ctx, 240, 1.0F);
    expect_boxes_rest_on_the_floor(demo);
    expect_no_box_in_the_panel(demo);
}

// Audit D2 through the real path: at 10x every frame is sliced, and bodies
// still land on the floor.
TEST(GpuDemoPhysics, BoxesLandAtTimeScaleTen) {
    QUAT_REQUIRE_GPU(ctx);
    const DemoResult demo = run_physics_demo(*ctx, 60, 10.0F);
    expect_boxes_rest_on_the_floor(demo);
    expect_no_box_in_the_panel(demo);
}

// The overlap test itself: a box straddling the panel overlaps, one beside
// it does not.
TEST(DemoPhysicsGeometry, BoxPanelOverlapTest) {
    const glm::vec3 panel_centre(0.0F, 2.2F, 3.0F);
    const glm::vec3 panel_half(3.0F, 2.2F, 0.08F);
    glm::mat4 box(1.0F);
    box[3] = glm::vec4(0.0F, 0.5F, 3.0F, 1.0F);
    EXPECT_TRUE(boxes_overlap(box, glm::vec3(k_box_half), panel_centre, panel_half, 0.03F));
    box[3] = glm::vec4(0.0F, 0.5F, 3.6F, 1.0F); // resting against the panel's face
    EXPECT_FALSE(boxes_overlap(box, glm::vec3(k_box_half), panel_centre, panel_half, 0.03F));
}

} // namespace
