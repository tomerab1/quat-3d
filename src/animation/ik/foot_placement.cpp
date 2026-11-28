// Foot placement: ground probes, pelvis offset, leg IK, foot alignment.

#include "engine/animation/foot_placement.hpp"
#include "engine/core/spring.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace engine::animation {

namespace {

constexpr glm::vec3 k_up(0.0F, 1.0F, 0.0F);
constexpr float     k_min_ground_normal_y = 0.2F; // steeper than ~78 deg is a wall

// Lower-case alphanumerics only: "mixamorig:LeftFoot" -> "mixamorigleftfoot".
[[nodiscard]] std::string normalized(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

[[nodiscard]] int find_joint(const SkeletonAsset& skeleton, std::string_view name) {
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        if (skeleton.joints[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// First joint whose normalized name ends with one of `suffixes`.
[[nodiscard]] int detect_joint(const SkeletonAsset& skeleton,
                               std::initializer_list<std::string_view> suffixes) {
    for (const std::string_view suffix : suffixes) {
        for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
            if (normalized(skeleton.joints[i].name).ends_with(suffix)) return static_cast<int>(i);
        }
    }
    return -1;
}

[[nodiscard]] int parent_of(const SkeletonAsset& skeleton, int joint) {
    return joint >= 0 && static_cast<std::size_t>(joint) < skeleton.joints.size()
               ? skeleton.joints[static_cast<std::size_t>(joint)].parent
               : -1;
}

[[nodiscard]] bool is_ancestor(const SkeletonAsset& skeleton, int ancestor, int joint) {
    for (std::size_t guard = 0; joint >= 0 && guard <= skeleton.joints.size(); ++guard) {
        if (joint == ancestor) return true;
        joint = parent_of(skeleton, joint);
    }
    return false;
}

[[nodiscard]] int common_ancestor(const SkeletonAsset& skeleton, int a, int b) {
    for (std::size_t guard = 0; a >= 0 && guard <= skeleton.joints.size(); ++guard) {
        if (is_ancestor(skeleton, a, b)) return a;
        a = parent_of(skeleton, a);
    }
    return -1;
}

// Critically damped spring toward `goal` with the given half-life
// (engine/core/spring.hpp: Holden, "Spring-It-On", 2021).
void spring(float& x, float& v, float goal, float half_life, float dt) {
    core::spring_damper(x, v, goal, half_life, dt);
}

} // namespace

FootRig find_foot_rig(const SkeletonAsset& skeleton, std::string_view pelvis,
                      std::string_view left_foot, std::string_view right_foot,
                      const glm::mat4& root_transform) {
    FootRig rig;
    const std::array<int, 2> feet{
        left_foot.empty()
            ? detect_joint(skeleton, {"leftfoot", "footl", "lfoot", "leftankle", "anklel"})
            : find_joint(skeleton, left_foot),
        right_foot.empty()
            ? detect_joint(skeleton, {"rightfoot", "footr", "rfoot", "rightankle", "ankler"})
            : find_joint(skeleton, right_foot)};
    for (std::size_t i = 0; i < 2; ++i) {
        rig.legs[i].end = feet[i];
        rig.legs[i].lower = parent_of(skeleton, feet[i]);
        rig.legs[i].upper = parent_of(skeleton, rig.legs[i].lower);
        if (!rig.legs[i].valid()) return FootRig{};
    }
    if (feet[0] == feet[1]) return FootRig{};
    rig.pelvis = pelvis.empty()
                     ? common_ancestor(skeleton, rig.legs[0].upper, rig.legs[1].upper)
                     : find_joint(skeleton, pelvis);
    if (rig.pelvis < 0 || !is_ancestor(skeleton, rig.pelvis, rig.legs[0].upper) ||
        !is_ancestor(skeleton, rig.pelvis, rig.legs[1].upper)) {
        return FootRig{};
    }

    rig.pelvis_subtree.assign(skeleton.joints.size(), 0);
    for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
        rig.pelvis_subtree[j] = is_ancestor(skeleton, rig.pelvis, static_cast<int>(j)) ? 1 : 0;
    }
    const std::vector<glm::mat4> bind = compute_bind_world(skeleton);
    for (std::size_t i = 0; i < 2; ++i) {
        const glm::vec4 ankle =
            root_transform * bind[static_cast<std::size_t>(rig.legs[i].end)][3];
        rig.ankle_height[i] = ankle.y - root_transform[3].y;
        // Bend direction in armature space (mapped to world per frame).
        const glm::vec3 hip(bind[static_cast<std::size_t>(rig.legs[i].upper)][3]);
        const glm::vec3 knee(bind[static_cast<std::size_t>(rig.legs[i].lower)][3]);
        const glm::vec3 rest_ankle(bind[static_cast<std::size_t>(rig.legs[i].end)][3]);
        const glm::vec3 bend = knee - 0.5F * (hip + rest_ankle);
        rig.bind_bend[i] = glm::length(bend) > 1e-5F ? glm::normalize(bend) : glm::vec3(0.0F);
    }
    return rig;
}

FootPlacementResult apply_foot_placement(const SkeletonAsset& skeleton, std::span<JointPose> pose,
                                         const glm::mat4& root_transform, const FootRig& rig,
                                         const FootPlacementSettings& settings,
                                         const GroundProbe& probe, FootPlacementState& state,
                                         float dt) {
    FootPlacementResult result;
    const std::size_t n = skeleton.joints.size();
    if (!rig.valid() || pose.size() != n || rig.pelvis_subtree.size() != n) return result;

    std::vector<glm::mat4> world = pose_world_matrices(skeleton, pose, root_transform);
    const float ground_ref = root_transform[3].y;
    const float max_adjust = std::max(settings.max_adjust, 0.0F);

    // Probe under each animated ankle.
    std::array<glm::vec3, 2> animated_ankle{};
    std::array<float, 2>     lift{};
    std::array<glm::vec3, 2> normal_goal{k_up, k_up};
    for (std::size_t i = 0; i < 2; ++i) {
        animated_ankle[i] = glm::vec3(world[static_cast<std::size_t>(rig.legs[i].end)][3]);
        lift[i] = animated_ankle[i].y - ground_ref - rig.ankle_height[i];
        const float up = std::max(settings.ray_up, 0.0F);
        const std::optional<GroundHit> hit =
            probe ? probe(animated_ankle[i] + k_up * up, up + std::max(settings.ray_down, 0.0F))
                  : std::nullopt;
        if (!hit) continue;
        const glm::vec3 nrm = glm::normalize(hit->normal);
        if (!(nrm.y > k_min_ground_normal_y)) continue;
        result.grounded[i] = true;
        result.foot_offset[i] = std::clamp(hit->point.y - ground_ref, -max_adjust, max_adjust);
        normal_goal[i] = nrm;
    }
    const float pelvis_goal = std::min(result.foot_offset[0], result.foot_offset[1]);

    if (!state.initialized || !(dt > 0.0F)) {
        if (!state.initialized) {
            state.pelvis = pelvis_goal;
            state.foot = result.foot_offset;
            state.normal = normal_goal;
            state.initialized = true;
        }
    } else {
        spring(state.pelvis, state.pelvis_velocity, pelvis_goal, settings.smoothing, dt);
        const float blend = settings.smoothing > 0.0F
                                ? 1.0F - std::exp(-0.69314718F * dt / settings.smoothing)
                                : 1.0F;
        for (std::size_t i = 0; i < 2; ++i) {
            spring(state.foot[i], state.foot_velocity[i], result.foot_offset[i],
                   settings.smoothing, dt);
            state.normal[i] = glm::normalize(glm::mix(state.normal[i], normal_goal[i], blend));
        }
    }
    result.pelvis_offset = state.pelvis;
    result.foot_offset = state.foot;

    // Pelvis: move it (and, in world space, everything under it) along +Y.
    const auto pelvis = static_cast<std::size_t>(rig.pelvis);
    const glm::vec3 delta = k_up * state.pelvis;
    const int pelvis_parent = skeleton.joints[pelvis].parent;
    const glm::mat4& parent_world =
        pelvis_parent >= 0 ? world[static_cast<std::size_t>(pelvis_parent)] : root_transform;
    pose[pelvis].translation += glm::inverse(glm::mat3(parent_world)) * delta;
    for (std::size_t j = 0; j < n; ++j) {
        if (rig.pelvis_subtree[j] != 0) world[j][3] += glm::vec4(delta, 0.0F);
    }

    const glm::vec3 forward =
        glm::normalize(glm::vec3(root_transform * glm::vec4(0.0F, 0.0F, 1.0F, 0.0F)));
    const float max_angle = std::max(settings.max_foot_angle, 0.0F);
    for (std::size_t i = 0; i < 2; ++i) {
        const TwoBoneChain& leg = rig.legs[i];
        const glm::vec3 a(world[static_cast<std::size_t>(leg.upper)][3]);
        const glm::vec3 b(world[static_cast<std::size_t>(leg.lower)][3]);
        const glm::vec3 c(world[static_cast<std::size_t>(leg.end)][3]);

        // Bend the knee the way the animation does; a straight leg keeps
        // last frame's bend, else the rest pose's, else forward.
        glm::vec3 rest_bend = glm::mat3(root_transform) * rig.bind_bend[i];
        if (glm::length(rest_bend) > 1e-8F) rest_bend = glm::normalize(rest_bend); // scaled armatures
        glm::vec3 hint = state.knee_bend[i];
        if (glm::length(hint) < 1e-4F) hint = rest_bend;
        if (glm::length(hint) < 1e-4F) hint = forward;
        glm::vec3 knee_dir = b - 0.5F * (a + c);
        if (glm::length(knee_dir) < 1e-4F) knee_dir = hint;
        const glm::vec3 pole = b + glm::normalize(knee_dir) * glm::length(b - a);

        glm::quat end_rotation(1.0F, 0.0F, 0.0F, 0.0F);
        const TwoBoneIkSolution solved =
            apply_two_bone_ik(pose, world, leg, animated_ankle[i] + k_up * state.foot[i], pole,
                              1.0F, &end_rotation, hint);
        const glm::vec3 bend = solved.joint - 0.5F * (a + solved.end);
        if (glm::length(bend) > 1e-4F) state.knee_bend[i] = glm::normalize(bend);

        // Planted feet tilt toward the ground normal, up to the limit.
        const float plant = result.grounded[i] && settings.plant_height > 0.0F
                                ? std::clamp(1.0F - lift[i] / settings.plant_height, 0.0F, 1.0F)
                                : 0.0F;
        glm::quat tilt = rotation_between(k_up, state.normal[i]);
        const float angle = 2.0F * std::acos(std::clamp(std::abs(tilt.w), 0.0F, 1.0F));
        if (angle > max_angle && angle > 0.0F) {
            tilt = glm::slerp(glm::quat(1.0F, 0.0F, 0.0F, 0.0F), tilt, max_angle / angle);
        }
        tilt = glm::slerp(glm::quat(1.0F, 0.0F, 0.0F, 0.0F), tilt, plant);
        // The foot keeps its animated world orientation (IK turned it with
        // the shin), tilted onto the ground.
        const glm::quat animated = world_rotation(world[static_cast<std::size_t>(leg.end)]);
        JointPose& foot = pose[static_cast<std::size_t>(leg.end)];
        foot.rotation =
            glm::normalize(foot.rotation * glm::inverse(end_rotation) * tilt * animated);
    }
    return result;
}

} // namespace engine::animation
