// Animator runtime: clip(s) -> pose -> blend -> skinning matrices (5.3 + 5.5).

#include "engine/animation/animator.hpp"

#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace engine::animation {

namespace {

[[nodiscard]] glm::mat4 compose(const JointPose& p) {
    return glm::translate(glm::mat4(1.0F), p.translation) * glm::mat4_cast(p.rotation) *
           glm::scale(glm::mat4(1.0F), p.scale);
}

[[nodiscard]] bool mask_enabled(std::span<const std::uint32_t> mask, std::size_t joint) {
    if (mask.empty()) return true;
    const std::size_t word = joint / 32;
    return word < mask.size() && ((mask[word] >> (joint % 32)) & 1U) != 0U;
}

} // namespace

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

    std::unordered_map<int, std::size_t> node_to_joint;
    node_to_joint.reserve(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        node_to_joint[skeleton.joints[i].node] = i;
    }

    for (const AnimChannel& ch : clip.channels) {
        const auto it = node_to_joint.find(ch.target_node);
        if (it == node_to_joint.end() || ch.sampler >= clip.samplers.size()) {
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
                                              const std::vector<JointPose>& pose) {
    std::vector<glm::mat4> local(pose.size());
    for (std::size_t i = 0; i < pose.size(); ++i) {
        local[i] = compose(pose[i]);
    }
    const std::vector<glm::mat4> world = compute_world_matrices(skeleton, local);
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

std::vector<glm::mat4> bind_skinning_matrices(const SkeletonAsset& skeleton) {
    return pose_skinning_matrices(skeleton, bind_pose(skeleton));
}

std::vector<glm::mat4> sample_skinning_matrices(const SkeletonAsset& skeleton,
                                                const AnimClipAsset& clip, float time) {
    return pose_skinning_matrices(skeleton, sample_local_pose(skeleton, clip, time));
}

// ---------------------------------------------------------------------------
// Self-tests
// ---------------------------------------------------------------------------
namespace {

// Two-joint skeleton (root node 0; child node 1 at +2 X) with inverse_bind set so
// the bind pose skins to identity.
[[nodiscard]] SkeletonAsset two_joint_skeleton() {
    SkeletonAsset skel;
    skel.joints.resize(2);
    skel.joints[0].name = "root";
    skel.joints[0].parent = -1;
    skel.joints[0].node = 0;
    skel.joints[1].name = "child";
    skel.joints[1].parent = 0;
    skel.joints[1].node = 1;
    skel.joints[1].translation = {2.0F, 0.0F, 0.0F};
    const std::vector<glm::mat4> bw = compute_bind_world(skel);
    for (std::size_t i = 0; i < skel.joints.size(); ++i) {
        skel.joints[i].inverse_bind = glm::inverse(bw[i]);
    }
    return skel;
}

// A clip that holds the child (node 1) at a constant translation.
[[nodiscard]] AnimClipAsset child_translation_clip(glm::vec3 t) {
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {glm::vec4(t, 0.0F), glm::vec4(t, 0.0F)};
    clip.samplers = {s};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};
    return clip;
}

} // namespace

std::expected<void, core::Error> run_animator_self_test() {
    const SkeletonAsset skel = two_joint_skeleton();

    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {{2, 0, 0, 0}, {5, 0, 0, 0}};
    clip.samplers = {s};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};

    const std::vector<glm::mat4> m0 = sample_skinning_matrices(skel, clip, 0.0F);
    for (const glm::mat4& m : m0) {
        if (glm::distance(m[3], glm::vec4(0, 0, 0, 1)) > 1e-5F) {
            return std::unexpected(core::Error{"animator self-test: bind-time skinning not identity"});
        }
    }
    const std::vector<glm::mat4> m1 = sample_skinning_matrices(skel, clip, 1.0F);
    if (glm::distance(glm::vec3(m1[1][3]), glm::vec3(3.0F, 0.0F, 0.0F)) > 1e-5F) {
        return std::unexpected(core::Error{"animator self-test: posed child displacement wrong"});
    }
    if (glm::distance(m1[0][3], glm::vec4(0, 0, 0, 1)) > 1e-5F) {
        return std::unexpected(core::Error{"animator self-test: unanimated root not identity"});
    }
    return {};
}

std::expected<void, core::Error> run_blend_self_test() {
    const SkeletonAsset skel = two_joint_skeleton();

    // 1D blend: clip_a holds child at bind (2,0,0), clip_b at (8,0,0). At blend
    // 0.5 the child sits at (5,0,0); skinning displaces a bind vertex by (3,0,0).
    const AnimClipAsset a = child_translation_clip({2.0F, 0.0F, 0.0F});
    const AnimClipAsset b = child_translation_clip({8.0F, 0.0F, 0.0F});

    BlendTree tree;
    tree.clip_a = &a;
    tree.clip_b = &b;
    tree.blend = 0.5F;
    std::vector<glm::mat4> m = tree.evaluate(skel);
    if (glm::distance(glm::vec3(m[1][3]), glm::vec3(3.0F, 0.0F, 0.0F)) > 1e-4F) {
        return std::unexpected(core::Error{"blend self-test: 1D blend midpoint wrong"});
    }

    // Additive: base = clip_a (child at bind). additive clip moves child to
    // (3,0,0) => delta from bind = (1,0,0). With weight 1 and the child enabled,
    // skinning displaces by (1,0,0); the root stays put.
    const AnimClipAsset add = child_translation_clip({3.0F, 0.0F, 0.0F});
    BlendTree additive_tree;
    additive_tree.clip_a = &a;
    additive_tree.additive = &add;
    additive_tree.additive_weight = 1.0F;
    additive_tree.additive_mask = {0x2U}; // only joint 1 (the child)
    m = additive_tree.evaluate(skel);
    if (glm::distance(glm::vec3(m[1][3]), glm::vec3(1.0F, 0.0F, 0.0F)) > 1e-4F) {
        return std::unexpected(core::Error{"blend self-test: additive layer wrong"});
    }
    if (glm::distance(m[0][3], glm::vec4(0, 0, 0, 1)) > 1e-4F) {
        return std::unexpected(core::Error{"blend self-test: additive leaked to masked-out root"});
    }

    // Masking the child out (enable only the root) leaves the child at bind.
    additive_tree.additive_mask = {0x1U};
    m = additive_tree.evaluate(skel);
    if (glm::distance(m[1][3], glm::vec4(0, 0, 0, 1)) > 1e-4F) {
        return std::unexpected(core::Error{"blend self-test: masked-out child was still affected"});
    }
    return {};
}

} // namespace engine::animation
