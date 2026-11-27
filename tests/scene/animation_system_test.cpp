// Animation system: Animator clip switches (inertialized / crossfaded), FootIK
// on a skinned entity, and the animator / foot_ik codec.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "animation/test_rig.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/scene/animation_system.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
namespace anim = engine::animation;
using Clip = engine::asset::AssetHandle<anim::AnimClipAsset>;

constexpr float k_dt = 1.0F / 60.0F;

[[nodiscard]] anim::AnimClipAsset slide_clip(const std::string& name, float from, float to) {
    anim::AnimClipAsset clip = quat::test::child_translation_clip({from, 0.0F, 0.0F});
    clip.name = name;
    clip.samplers[0].output[1] = glm::vec4(to, 0.0F, 0.0F, 0.0F);
    return clip;
}

// A two-joint skinned entity whose animator can switch between "slide"
// (child x 2 -> 4 over 1 s) and "hold" (child at x = 2.5).
class AnimatorSwitch : public ::testing::Test {
protected:
    engine::asset::AssetManager assets;
    entt::registry              reg;
    entt::entity                e = reg.create();

    void SetUp() override {
        const auto skel = assets.load<anim::SkeletonAsset>(
            "rig", [](const auto&) { return quat::test::two_joint_skeleton(); });
        const Clip slide = assets.load<anim::AnimClipAsset>(
            "slide", [](const auto&) { return slide_clip("slide", 2.0F, 4.0F); });
        const Clip hold = assets.load<anim::AnimClipAsset>(
            "hold", [](const auto&) { return slide_clip("hold", 2.5F, 2.5F); });
        reg.emplace<s::Transform>(e);
        reg.emplace<s::SkinnedMesh>(e, skel, std::vector<glm::mat4>{});
        s::Animator& a = reg.emplace<s::Animator>(e);
        a.clip = slide;
        a.clips = {slide, hold};
        a.looping = false;
        a.blend_time = 0.5F;
    }

    [[nodiscard]] float child_x() const {
        return quat::test::displacement(reg.get<s::SkinnedMesh>(e).joint_matrices[1]).x + 2.0F;
    }
    float tick() {
        s::animation_system(reg, k_dt);
        return child_x();
    }
};

TEST_F(AnimatorSwitch, InertializedSwitchIsContinuousAndSettlesOnTheNewClip) {
    reg.get<s::Animator>(e).time = 0.5F - 30 * k_dt;
    std::vector<float> xs;
    for (int i = 0; i < 30; ++i) xs.push_back(tick());
    ASSERT_NEAR(xs.back(), 3.0F, 1e-4F);

    s::Animator& a = reg.get<s::Animator>(e);
    a.clip = a.clips[1];
    a.time = 0.0F;
    xs.push_back(tick());
    EXPECT_TRUE(reg.get<s::AnimationRuntime>(e).inertializer.active());
    const std::size_t n = xs.size();
    EXPECT_NEAR((xs[n - 1] - xs[n - 2]) / k_dt, (xs[n - 2] - xs[n - 3]) / k_dt, 0.1F)
        << "velocity jumps at the clip switch";

    float x = 0.0F;
    for (int i = 0; i < 31; ++i) x = tick();
    EXPECT_FALSE(reg.get<s::AnimationRuntime>(e).inertializer.active());
    EXPECT_NEAR(x, 2.5F, 1e-5F);
}

TEST_F(AnimatorSwitch, CrossfadeBlendsTheOldClipOut) {
    reg.get<s::Animator>(e).time = 0.5F - k_dt;
    (void)tick(); // slide at t = 0.5: x = 3
    s::Animator& a = reg.get<s::Animator>(e);
    a.blend_mode = anim::BlendMode::crossfade;
    a.speed = 0.0F; // freeze both clocks: the blend alone moves the pose
    a.clip = a.clips[1];
    EXPECT_NEAR(tick(), 3.0F, 1e-4F) << "crossfade starts on the old pose";
    for (int i = 0; i < 15; ++i) (void)tick();
    EXPECT_NEAR(child_x(), 3.0F + (2.5F - 3.0F) * 0.5F, 1e-3F) << "halfway";
    for (int i = 0; i < 16; ++i) (void)tick();
    EXPECT_NEAR(child_x(), 2.5F, 1e-5F);
}

// Switching again mid-transition must not pop: the per-tick change on the
// second switch stays within the motion of the ticks before it.
class AnimatorReswitch : public AnimatorSwitch {
protected:
    void SetUp() override {
        AnimatorSwitch::SetUp();
        reg.get<s::Animator>(e).clips.push_back(assets.load<anim::AnimClipAsset>(
            "far", [](const auto&) { return slide_clip("far", 1.0F, 1.0F); }));
        reg.get<s::Animator>(e).time = 0.5F - 30 * k_dt;
        for (int i = 0; i < 30; ++i) (void)tick(); // slide at t = 0.5: x = 3, +2 m/s
    }

    // Switches to clip `index` with `mode`, ticks `frames` times; returns x.
    std::vector<float> switch_to(std::size_t index, anim::BlendMode mode, int frames) {
        s::Animator& a = reg.get<s::Animator>(e);
        a.blend_mode = mode;
        a.clip = a.clips[index];
        a.time = 0.0F;
        std::vector<float> xs;
        for (int i = 0; i < frames; ++i) xs.push_back(tick());
        return xs;
    }

    static void expect_no_pop(const std::vector<float>& before, float after) {
        const std::size_t n = before.size();
        const float step = std::abs(before[n - 1] - before[n - 2]);
        EXPECT_LE(std::abs(after - before[n - 1]), step + 0.01F)
            << "popped from " << before[n - 1] << " to " << after << " (ticks moved " << step
            << ")";
    }
};

TEST_F(AnimatorReswitch, CrossfadeDuringAnInertializationDoesNotPop) {
    const std::vector<float> xs = switch_to(1, anim::BlendMode::inertialize, 10);
    ASSERT_TRUE(reg.get<s::AnimationRuntime>(e).inertializer.active());
    const std::vector<float> next = switch_to(2, anim::BlendMode::crossfade, 1);
    expect_no_pop(xs, next[0]);
    EXPECT_NEAR(switch_to(2, anim::BlendMode::crossfade, 31).back(), 1.0F, 1e-5F);
}

TEST_F(AnimatorReswitch, CrossfadeDuringACrossfadeDoesNotPop) {
    const std::vector<float> xs = switch_to(1, anim::BlendMode::crossfade, 15);
    ASSERT_TRUE(reg.get<s::AnimationRuntime>(e).fade_from.valid());
    const std::vector<float> next = switch_to(2, anim::BlendMode::crossfade, 1);
    expect_no_pop(xs, next[0]);
    EXPECT_NEAR(switch_to(2, anim::BlendMode::crossfade, 31).back(), 1.0F, 1e-5F);
}

TEST_F(AnimatorSwitch, CodecSwitchesClipsByNameAndListsThem) {
    nlohmann::json enc = s::encode_components(reg, e);
    ASSERT_TRUE(enc.contains("animator"));
    EXPECT_EQ(enc["animator"]["clip"], "slide");
    EXPECT_EQ(enc["animator"]["clips"], nlohmann::json::array({"slide", "hold"}));
    EXPECT_EQ(enc["animator"]["blend"], "inertialize");

    reg.get<s::Animator>(e).time = 0.7F;
    EXPECT_OK(s::apply_component(reg, e, "animator",
                                 {{"clip", "hold"}, {"blend", "crossfade"}, {"blend_time", 0.1}},
                                 /*strict=*/true));
    const s::Animator& a = reg.get<s::Animator>(e);
    EXPECT_EQ(a.clip.id(), a.clips[1].id());
    EXPECT_EQ(a.time, 0.0F) << "a new clip starts at its beginning";
    EXPECT_EQ(a.blend_mode, anim::BlendMode::crossfade);
    EXPECT_FLOAT_EQ(a.blend_time, 0.1F);

    const auto bad = s::apply_component(reg, e, "animator", {{"clip", "run"}}, true);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find("available: slide, hold"), std::string::npos)
        << bad.error().message;
    EXPECT_OK(s::apply_component(reg, e, "animator", {{"clip", "run"}}, /*strict=*/false))
        << "scene files skip clips that no longer exist";
}

// Pelvis -> two legs (hip, knee, ankle); ankles 0.1 m above the root plane.
[[nodiscard]] anim::SkeletonAsset legs() {
    anim::SkeletonAsset skel;
    const auto add = [&](const char* name, int parent, glm::vec3 t) {
        anim::Joint j;
        j.name = name;
        j.parent = parent;
        j.node = static_cast<int>(skel.joints.size());
        j.translation = t;
        skel.joints.push_back(j);
    };
    add("Hips", -1, {0.0F, 1.0F, 0.0F});
    add("LeftUpLeg", 0, {0.1F, 0.0F, 0.0F});
    add("LeftLeg", 1, {0.0F, -0.45F, 0.08F});
    add("LeftFoot", 2, {0.0F, -0.45F, -0.08F});
    add("RightUpLeg", 0, {-0.1F, 0.0F, 0.0F});
    add("RightLeg", 4, {0.0F, -0.45F, 0.08F});
    add("RightFoot", 5, {0.0F, -0.45F, -0.08F});
    const std::vector<glm::mat4> bind = anim::compute_bind_world(skel);
    for (std::size_t i = 0; i < skel.joints.size(); ++i) {
        skel.joints[i].inverse_bind = glm::inverse(bind[i]);
    }
    return skel;
}

TEST(FootIkSystem, LowersThePelvisOntoAStepAndIgnoresItsOwnColliders) {
    engine::asset::AssetManager assets;
    entt::registry              reg;
    const entt::entity          e = reg.create();
    reg.emplace<s::Transform>(e);
    reg.emplace<s::SkinnedMesh>(
        e, assets.load<anim::SkeletonAsset>("legs", [](const auto&) { return legs(); }),
        std::vector<glm::mat4>{});
    reg.emplace<s::Animator>(e); // no clip: the bind pose
    reg.emplace<s::FootIK>(e);

    bool saw_self = false;
    const s::FootGroundProbe step = [&](const glm::vec3& origin, float length,
                                        std::span<const entt::entity> ignore)
        -> std::optional<anim::GroundHit> {
        saw_self = !ignore.empty() && ignore[0] == e;
        const float h = origin.x > 0.0F ? 0.0F : -0.2F;
        if (h > origin.y || h < origin.y - length) return std::nullopt;
        return anim::GroundHit{{origin.x, h, origin.z}, {0.0F, 1.0F, 0.0F}};
    };
    s::animation_system(reg, k_dt, step);
    EXPECT_TRUE(saw_self);

    // Skinning matrices displace bind-pose vertices: the pelvis and the
    // right foot drop 0.2 m, the left foot stays planted.
    const std::vector<glm::mat4>& m = reg.get<s::SkinnedMesh>(e).joint_matrices;
    ASSERT_EQ(m.size(), 7U);
    EXPECT_NEAR(quat::test::displacement(m[0]).y, -0.2F, 1e-4F);
    const auto ankle_y = [&](std::size_t j) { return (m[j] * glm::inverse(legs().joints[j].inverse_bind))[3].y; };
    EXPECT_NEAR(ankle_y(3), 0.1F, 1e-4F);
    EXPECT_NEAR(ankle_y(6), -0.1F, 1e-4F);
    EXPECT_TRUE(reg.get<s::AnimationRuntime>(e).foot_rig.valid());

    // Disabled: back to the animated (bind) pose.
    reg.get<s::FootIK>(e).enabled = false;
    s::animation_system(reg, k_dt, step);
    EXPECT_NEAR(quat::test::displacement(reg.get<s::SkinnedMesh>(e).joint_matrices[0]).y, 0.0F,
                1e-6F);
}

TEST(FootIkSystem, ProbesSkipEverythingTheCharacterOwns) {
    // world root > character (controller) > model (skinned) > weapon (collider);
    // a sibling crate under the world root is ground.
    engine::asset::AssetManager assets;
    s::Scene                    scene;
    entt::registry&             reg = scene.registry();
    const entt::entity world = scene.create_entity("world");
    const entt::entity character = scene.create_entity("character");
    const entt::entity model = scene.create_entity("model");
    const entt::entity weapon = scene.create_entity("weapon");
    const entt::entity crate = scene.create_entity("crate");
    ASSERT_TRUE(scene.set_parent(character, world));
    ASSERT_TRUE(scene.set_parent(model, character));
    ASSERT_TRUE(scene.set_parent(weapon, model));
    ASSERT_TRUE(scene.set_parent(crate, world));
    reg.emplace<s::CharacterController>(character);
    reg.emplace<s::Collider>(weapon);
    reg.emplace<s::SkinnedMesh>(
        model, assets.load<anim::SkeletonAsset>("legs", [](const auto&) { return legs(); }),
        std::vector<glm::mat4>{});
    reg.emplace<s::Animator>(model);
    reg.emplace<s::FootIK>(model);

    std::vector<entt::entity> seen;
    const s::FootGroundProbe probe = [&](const glm::vec3&, float,
                                         std::span<const entt::entity> ignore)
        -> std::optional<anim::GroundHit> {
        seen.assign(ignore.begin(), ignore.end());
        return std::nullopt;
    };
    s::animation_system(reg, k_dt, probe);
    const auto has = [&](entt::entity x) { return std::find(seen.begin(), seen.end(), x) != seen.end(); };
    EXPECT_TRUE(has(character));
    EXPECT_TRUE(has(model));
    EXPECT_TRUE(has(weapon)) << "a collider parented under the character catches the feet";
    EXPECT_FALSE(has(world));
    EXPECT_FALSE(has(crate));
}

TEST(FootIkCodec, RoundTripsAndValidates) {
    entt::registry     reg;
    const entt::entity e = reg.create();
    reg.emplace<s::Transform>(e);
    EXPECT_OK(s::apply_component(reg, e, "foot_ik",
                                 {{"left_foot", "foot_l"}, {"max_adjust", 0.3}, {"enabled", false}},
                                 true));
    const s::FootIK& k = reg.get<s::FootIK>(e);
    EXPECT_EQ(k.left_foot, "foot_l");
    EXPECT_FLOAT_EQ(k.max_adjust, 0.3F);
    EXPECT_FALSE(k.enabled);
    const nlohmann::json enc = s::encode_components(reg, e);
    EXPECT_EQ(enc["foot_ik"]["left_foot"], "foot_l");
    EXPECT_FALSE(s::apply_component(reg, e, "foot_ik", {{"max_foot_angle", 120}}, true));
    EXPECT_FALSE(s::apply_component(reg, e, "foot_ik", {{"bogus", 1}}, true));
    EXPECT_TRUE(s::remove_component(reg, e, "foot_ik"));
}

} // namespace
