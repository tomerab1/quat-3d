// Animator runtime: clip(s) -> pose -> blend -> skinning matrices (5.3 + 5.5).

#include "engine/animation/animator.hpp"

#include <algorithm>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/animation/compression.hpp"

namespace engine::animation {

namespace {

[[nodiscard]] bool mask_enabled(std::span<const std::uint32_t> mask, std::size_t joint) {
    if (mask.empty()) return true;
    const std::size_t word = joint / 32;
    return word < mask.size() && ((mask[word] >> (joint % 32)) & 1U) != 0U;
}

} // namespace

glm::mat4 joint_matrix(const JointPose& p) {
    return glm::translate(glm::mat4(1.0F), p.translation) * glm::mat4_cast(p.rotation) *
           glm::scale(glm::mat4(1.0F), p.scale);
}

std::vector<glm::mat4> pose_world_matrices(const SkeletonAsset& skeleton,
                                           std::span<const JointPose> pose,
                                           const glm::mat4& root_transform) {
    std::vector<glm::mat4> local(pose.size());
    for (std::size_t i = 0; i < pose.size(); ++i) {
        local[i] = joint_matrix(pose[i]);
    }
    return compute_world_matrices(skeleton, local, root_transform);
}

std::vector<JointPose> bind_pose(const SkeletonAsset& skeleton) {
    std::vector<JointPose> pose(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        pose[i].translation = skeleton.joints[i].translation;
        pose[i].rotation = skeleton.joints[i].rotation;
        pose[i].scale = skeleton.joints[i].scale;
    }
    return pose;
}

std::vector<JointPose> sample_local_pose(const SkeletonAsset& skeleton, const AnimClipAsset& clip,
                                         float time) {
    std::vector<JointPose> pose = bind_pose(skeleton);
    // Compressed tracks serve any skin over the clip's nodes. With the raw
    // keys dropped every animated node has a track (joints without one are
    // unanimated); with them kept, a skin the tracks do not cover reads them.
    if (clip.compressed != nullptr &&
        (clip.channels.empty() || clip.compressed->covers(skeleton))) {
        decompress_pose(*clip.compressed, skeleton, time, pose);
        return pose;
    }

    // node -> joint, sorted for binary search (cheaper than hashing here).
    std::vector<std::pair<int, std::size_t>> node_to_joint(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        node_to_joint[i] = {skeleton.joints[i].node, i};
    }
    std::sort(node_to_joint.begin(), node_to_joint.end());

    for (const AnimChannel& ch : clip.channels) {
        const auto it = std::lower_bound(node_to_joint.begin(), node_to_joint.end(),
                                         std::pair<int, std::size_t>{ch.target_node, 0});
        if (it == node_to_joint.end() || it->first != ch.target_node ||
            ch.sampler >= clip.samplers.size()) {
            continue;
        }
        const std::size_t j = it->second;
        const AnimSampler& s = clip.samplers[ch.sampler];
        switch (ch.path) {
        case AnimPath::translation:
            pose[j].translation = s.sample_vec3(time);
            break;
        case AnimPath::rotation:
            pose[j].rotation = s.sample_quat(time);
            break;
        case AnimPath::scale:
            pose[j].scale = s.sample_vec3(time);
            break;
        }
    }
    return pose;
}

std::vector<JointPose> blend_poses(const std::vector<JointPose>& a, const std::vector<JointPose>& b,
                                   float weight) {
    const std::size_t n = a.size();
    std::vector<JointPose> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i].translation = glm::mix(a[i].translation, b[i].translation, weight);
        out[i].rotation = glm::normalize(glm::slerp(a[i].rotation, b[i].rotation, weight));
        out[i].scale = glm::mix(a[i].scale, b[i].scale, weight);
    }
    return out;
}

std::vector<JointPose> apply_additive(const std::vector<JointPose>& base,
                                      const std::vector<JointPose>& additive,
                                      const std::vector<JointPose>& reference, float weight,
                                      std::span<const std::uint32_t> mask) {
    const std::size_t n = base.size();
    const glm::quat identity(1.0F, 0.0F, 0.0F, 0.0F);
    std::vector<JointPose> out = base;
    for (std::size_t i = 0; i < n; ++i) {
        if (!mask_enabled(mask, i)) continue;

        const glm::vec3 delta_t = additive[i].translation - reference[i].translation;
        const glm::quat delta_r = glm::inverse(reference[i].rotation) * additive[i].rotation;
        const glm::vec3 delta_s = additive[i].scale / reference[i].scale;

        out[i].translation = base[i].translation + weight * delta_t;
        out[i].rotation =
            glm::normalize(base[i].rotation * glm::slerp(identity, delta_r, weight));
        out[i].scale = base[i].scale * glm::mix(glm::vec3(1.0F), delta_s, weight);
    }
    return out;
}

std::vector<glm::mat4> pose_skinning_matrices(const SkeletonAsset& skeleton,
                                              const std::vector<JointPose>& pose,
                                              const glm::mat4& root_transform) {
    const std::vector<glm::mat4> world = pose_world_matrices(skeleton, pose, root_transform);
    std::vector<glm::mat4> out(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        out[i] = world[i] * skeleton.joints[i].inverse_bind;
    }
    return out;
}

std::vector<glm::mat4> BlendTree::evaluate(const SkeletonAsset& skeleton) const {
    std::vector<JointPose> pose =
        clip_a != nullptr ? sample_local_pose(skeleton, *clip_a, time) : bind_pose(skeleton);
    if (clip_b != nullptr) {
        pose = blend_poses(pose, sample_local_pose(skeleton, *clip_b, time), blend);
    }
    if (additive != nullptr) {
        pose = apply_additive(pose, sample_local_pose(skeleton, *additive, time),
                              bind_pose(skeleton), additive_weight, additive_mask);
    }
    return pose_skinning_matrices(skeleton, pose);
}

std::vector<glm::mat4> bind_skinning_matrices(const SkeletonAsset& skeleton,
                                              const glm::mat4& root_transform) {
    return pose_skinning_matrices(skeleton, bind_pose(skeleton), root_transform);
}

std::vector<glm::mat4> sample_skinning_matrices(const SkeletonAsset& skeleton,
                                                const AnimClipAsset& clip, float time,
                                                const glm::mat4& root_transform) {
    return pose_skinning_matrices(skeleton, sample_local_pose(skeleton, clip, time), root_transform);
}

} // namespace engine::animation
