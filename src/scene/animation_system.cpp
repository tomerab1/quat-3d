// Animation system: playback, clip transitions, foot placement, skinning.

#include "engine/scene/animation_system.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

using animation::JointPose;
using ClipHandle = asset::AssetHandle<animation::AnimClipAsset>;

[[nodiscard]] bool playable(const ClipHandle& clip) {
    return clip.valid() && clip.is_loaded() && clip->valid();
}

[[nodiscard]] float wrap_time(float time, float duration, bool looping) {
    if (!(duration > 0.0F)) return time;
    if (looping) {
        time = std::fmod(time, duration);
        return time < 0.0F ? time + duration : time;
    }
    return std::clamp(time, 0.0F, duration);
}

[[nodiscard]] std::vector<JointPose> sample(const animation::SkeletonAsset& skeleton,
                                            const ClipHandle& clip, float time) {
    return playable(clip) ? animation::sample_local_pose(skeleton, *clip, time)
                          : animation::bind_pose(skeleton);
}

// Starts a transition after `anim.clip` replaced `rt.playing`; returns this
// tick's pose when the transition already sampled it.
[[nodiscard]] std::vector<JointPose> begin_switch(const animation::SkeletonAsset& skeleton,
                                                  const Animator& anim, AnimationRuntime& rt,
                                                  float dt) {
    rt.inertializer.reset();
    rt.fade_from = {};
    const std::size_t joints = skeleton.joints.size();
    if (!(anim.blend_time > 0.0F)) return {};

    if (anim.blend_mode == animation::BlendMode::crossfade) {
        if (!playable(rt.playing)) return {};
        rt.fade_from = rt.playing;
        rt.fade_time = rt.playing_time;
        rt.fade_elapsed = 0.0F;
        rt.fade_duration = anim.blend_time;
        return {};
    }
    if (rt.last_pose.size() != joints || rt.prev_pose.size() != joints) return {};
    const float duration = playable(anim.clip) ? anim.clip->duration : 0.0F;
    std::vector<JointPose> now = sample(skeleton, anim.clip, anim.time);
    const std::vector<JointPose> before =
        sample(skeleton, anim.clip, wrap_time(anim.time - dt * anim.speed, duration, anim.looping));
    rt.inertializer.begin(rt.last_pose, rt.prev_pose, rt.last_dt, before, now, dt,
                          anim.blend_time);
    rt.inertializer.advance(dt);
    return now;
}

[[nodiscard]] std::vector<JointPose> animate(const animation::SkeletonAsset& skeleton,
                                             Animator& anim, AnimationRuntime& rt, float dt) {
    const bool has_clip = playable(anim.clip);
    if (has_clip) {
        anim.time = wrap_time(anim.time + dt * anim.speed, anim.clip->duration, anim.looping);
    }
    // The outgoing clip keeps its own clock while it fades out.
    if (rt.fade_from.valid()) {
        rt.fade_elapsed += dt;
        rt.fade_time = wrap_time(rt.fade_time + dt * anim.speed,
                                 playable(rt.fade_from) ? rt.fade_from->duration : 0.0F,
                                 anim.looping);
        if (rt.fade_elapsed >= rt.fade_duration) rt.fade_from = {};
    }
    if (rt.inertializer.active()) rt.inertializer.advance(dt);

    std::vector<JointPose> pose;
    const bool switched = rt.playing.valid() && anim.clip.id() != rt.playing.id();
    if (switched) {
        // The previous clip's clock, as if it had played this tick too.
        rt.playing_time = wrap_time(rt.playing_time + dt * anim.speed,
                                    playable(rt.playing) ? rt.playing->duration : 0.0F,
                                    anim.looping);
        pose = begin_switch(skeleton, anim, rt, dt);
    }
    if (pose.empty()) pose = sample(skeleton, anim.clip, anim.time);

    if (rt.fade_from.valid() && rt.fade_duration > 0.0F) {
        const float w = std::clamp(rt.fade_elapsed / rt.fade_duration, 0.0F, 1.0F);
        pose = animation::blend_poses(sample(skeleton, rt.fade_from, rt.fade_time), pose, w);
    }
    rt.inertializer.apply(pose);
    rt.playing = anim.clip;
    rt.playing_time = anim.time;
    return pose;
}

// The entity and its ancestors: colliders foot probes must not hit.
[[nodiscard]] std::size_t probe_ignores(const entt::registry& registry, entt::entity e,
                                        std::array<entt::entity, 8>& out) {
    std::size_t n = 0;
    for (entt::entity a = e; a != entt::null && n < out.size();) {
        out[n++] = a;
        const auto* parent = registry.try_get<Parent>(a);
        a = parent != nullptr && registry.valid(parent->entity) ? parent->entity : entt::null;
    }
    return n;
}

void place_feet(entt::registry& registry, entt::entity e, const FootIK& fik,
                const animation::SkeletonAsset& skeleton, std::vector<JointPose>& pose,
                const glm::mat4& root, AnimationRuntime& rt, const FootGroundProbe& ground,
                float dt) {
    const std::string names = fik.pelvis + '|' + fik.left_foot + '|' + fik.right_foot;
    if (rt.foot_rig_skeleton != &skeleton || rt.foot_rig_names != names) {
        rt.foot_rig = animation::find_foot_rig(skeleton, fik.pelvis, fik.left_foot,
                                               fik.right_foot, root);
        rt.foot_rig_skeleton = &skeleton;
        rt.foot_rig_names = names;
        rt.feet = {};
    }
    if (!rt.foot_rig.valid()) return;

    animation::FootPlacementSettings settings;
    settings.ray_up = fik.ray_up;
    settings.ray_down = fik.ray_down;
    settings.max_adjust = fik.max_adjust;
    settings.max_foot_angle = glm::radians(fik.max_foot_angle);
    settings.smoothing = fik.smoothing;

    std::array<entt::entity, 8> ignore{};
    const std::size_t ignored = probe_ignores(registry, e, ignore);
    const animation::GroundProbe probe =
        ground ? animation::GroundProbe(
                     [&](const glm::vec3& origin, float length) {
                         return ground(origin, length,
                                       std::span<const entt::entity>(ignore.data(), ignored));
                     })
               : animation::GroundProbe{};
    rt.foot_result = animation::apply_foot_placement(skeleton, pose, root, rt.foot_rig, settings,
                                                     probe, rt.feet, dt);
}

} // namespace

void animation_system(entt::registry& registry, float dt, const FootGroundProbe& ground) {
    for (auto [e, anim, skinned, transform] :
         registry.view<Animator, SkinnedMesh, Transform>().each()) {
        if (!skinned.skeleton.valid() || !skinned.skeleton.is_loaded()) {
            continue;
        }
        const animation::SkeletonAsset& skeleton = *skinned.skeleton;
        AnimationRuntime& rt = registry.get_or_emplace<AnimationRuntime>(e);
        if (rt.last_pose.size() != skeleton.joints.size()) rt = AnimationRuntime{};

        // The armature root in world space: the entity's current world transform
        // (fresh — transform_system runs first) times the load-time offset from
        // the entity's frame to the armature. Skinned draws use an identity
        // model matrix, so this is what places (and moves) the model.
        const glm::mat4 root = transform.world * skinned.root_transform;

        std::vector<JointPose> pose = animate(skeleton, anim, rt, dt);
        rt.prev_pose = std::move(rt.last_pose);
        rt.last_pose = pose;
        rt.last_dt = dt;

        if (const auto* fik = registry.try_get<FootIK>(e); fik != nullptr && fik->enabled) {
            place_feet(registry, e, *fik, skeleton, pose, root, rt, ground, dt);
        } else {
            rt.feet = {};
        }
        skinned.joint_matrices = animation::pose_skinning_matrices(skeleton, pose, root);
    }
}

} // namespace engine::scene
