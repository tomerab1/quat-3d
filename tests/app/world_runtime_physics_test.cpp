// The physics demo (QUAT_PHYSICS=1) stepped through the frame loop's own
// simulation path — WorldRuntime::build_scene, then WorldRuntime::simulate
// with the SimClock's dt each frame, exactly as Application does — must
// leave every box resting on the floor (issue #21: the boxes were created
// at the origin inside the floor and fell out of the world).

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "app/world_runtime.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/core/sim_clock.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/gpu_context.hpp"

namespace {

constexpr float k_real_dt = 1.0F / 60.0F;
constexpr std::size_t k_boxes = 12; // build_demo_scene's pile
constexpr float k_floor_top = 0.0F;

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

// Runs the physics demo for `frames` frames at `time_scale` and returns the
// boxes' world heights.
std::vector<float> run_physics_demo(quat::test::GpuContext& ctx, int frames, float time_scale) {
    engine::scene::Scene scene;
    engine::asset::AssetManager assets;
    quat::app::WorldRuntime world;
    {
        const ScopedEnv physics("QUAT_PHYSICS", "1");
        EXPECT_TRUE(world.build_scene(scene, assets, ctx.allocator, ctx.transfer));
    }
    EXPECT_TRUE(world.physics_mode());

    engine::core::SimClock clock;
    clock.set_time_scale(time_scale);
    for (int frame = 0; frame < frames; ++frame) {
        world.simulate(scene, static_cast<std::uint64_t>(frame), k_real_dt,
                       clock.advance(k_real_dt), nullptr);
    }

    std::vector<float> heights;
    for (auto [e, name, t] :
         scene.registry().view<engine::scene::Name, engine::scene::Transform>().each()) {
        if (name.value == "box") heights.push_back(t.world[3].y);
    }
    return heights;
}

void expect_boxes_rest_on_the_floor(const std::vector<float>& heights) {
    ASSERT_EQ(heights.size(), k_boxes);
    for (std::size_t i = 0; i < heights.size(); ++i) {
        // A box centre is at least half a box (0.5 m) above the floor when
        // resting on it; the pile is at most a few boxes high.
        EXPECT_GE(heights[i], k_floor_top + 0.4F) << "box " << i << " fell through the floor";
        EXPECT_LE(heights[i], k_floor_top + 15.0F) << "box " << i << " never landed";
    }
}

TEST(GpuDemoPhysics, BoxesComeToRestOnTheFloor) {
    QUAT_REQUIRE_GPU(ctx);
    // 4 simulated seconds: the highest box starts 15.65 m up.
    expect_boxes_rest_on_the_floor(run_physics_demo(*ctx, 240, 1.0F));
}

// Audit D2 through the real path: at 10x every frame is sliced, and bodies
// still land on the floor.
TEST(GpuDemoPhysics, BoxesLandAtTimeScaleTen) {
    QUAT_REQUIRE_GPU(ctx);
    expect_boxes_rest_on_the_floor(run_physics_demo(*ctx, 60, 10.0F));
}

} // namespace
