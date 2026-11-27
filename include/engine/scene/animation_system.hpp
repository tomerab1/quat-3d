#pragma once

// Animation system: clip playback with inertialized / crossfaded clip
// switches, foot placement, and skinning matrices (Phase 5, G8).
//
// Per skinned entity and tick: advance the Animator, sample its clip into a
// local pose (applying any running transition), run FootIK on that pose,
// then compose SkinnedMesh::joint_matrices under the entity's world root.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/animation/animator.hpp"
#include "engine/animation/foot_placement.hpp"
#include "engine/animation/inertialization.hpp"
#include "engine/asset/asset_manager.hpp"

namespace engine::scene {

// Ground under a foot: cast straight down from `origin` for `length` metres,
// skipping colliders owned by `ignore` (the character's whole hierarchy:
// its body or controller entity and everything parented under it).
using FootGroundProbe = std::function<std::optional<animation::GroundHit>(
    const glm::vec3& origin, float length, std::span<const entt::entity> ignore)>;

// System-managed per-entity animation state (not serialized; recreated on
// demand). Holds the pose history inertialization starts from, a running
// crossfade, and the resolved foot rig with its smoothed offsets.
struct AnimationRuntime {
    std::vector<animation::JointPose> last_pose; // previous tick's pose (before FootIK)
    std::vector<animation::JointPose> prev_pose; // the one before it
    float                             last_dt = 0.0F;
    animation::Inertializer           inertializer;

    asset::AssetHandle<animation::AnimClipAsset> playing; // clip of the last tick
    float                                        playing_time = 0.0F;
    asset::AssetHandle<animation::AnimClipAsset> fade_from; // crossfade source
    float                                        fade_time = 0.0F;
    float                                        fade_elapsed = 0.0F;
    float                                        fade_duration = 0.0F;

    animation::FootRig                foot_rig;
    const animation::SkeletonAsset*   foot_rig_skeleton = nullptr;
    std::string                       foot_rig_names; // pelvis|left|right it resolved
    animation::FootPlacementState     feet;
    animation::FootPlacementResult    foot_result;
    std::vector<entt::entity>         probe_ignore; // reused buffer for the probes
};

// Advance every Animator by `dt`, sample it against the entity's SkinnedMesh
// skeleton (switching clips per Animator::blend_mode), apply FootIK where
// present with `ground` probing, and write SkinnedMesh::joint_matrices.
void animation_system(entt::registry& registry, float dt, const FootGroundProbe& ground = {});

} // namespace engine::scene
