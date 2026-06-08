// Animator runtime: clip -> per-joint pose -> skinning matrices (Phase 5, 5.3).

#include "engine/animation/animator.hpp"

#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace engine::animation {

namespace {

// Per-joint local matrices for the skeleton posed by `clip` at `time`. Starts
// from each joint's bind TRS and overrides the components driven by a channel.
[[nodiscard]] std::vector<glm::mat4>
sample_local_matrices(const SkeletonAsset& skeleton, const AnimClipAsset& clip, float time) {
    const std::size_t n = skeleton.joints.size();

    // Pose seeded from the bind TRS.
    std::vector<glm::vec3> translation(n);
    std::vector<glm::quat> rotation(n);
    std::vector<glm::vec3> scale(n);
    for (std::size_t i = 0; i < n; ++i) {
        translation[i] = skeleton.joints[i].translation;
        rotation[i] = skeleton.joints[i].rotation;
        scale[i] = skeleton.joints[i].scale;
    }

    // Map the glTF node a channel targets onto a joint index.
    std::unordered_map<int, std::size_t> node_to_joint;
    node_to_joint.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
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
            translation[j] = s.sample_vec3(time);
            break;
        case AnimPath::rotation:
            rotation[j] = s.sample_quat(time);
            break;
        case AnimPath::scale:
            scale[j] = s.sample_vec3(time);
            break;
        }
    }

    std::vector<glm::mat4> local(n);
    for (std::size_t i = 0; i < n; ++i) {
        local[i] = glm::translate(glm::mat4(1.0F), translation[i]) * glm::mat4_cast(rotation[i]) *
                   glm::scale(glm::mat4(1.0F), scale[i]);
    }
    return local;
}

[[nodiscard]] std::vector<glm::mat4>
skinning_from_world(const SkeletonAsset& skeleton, const std::vector<glm::mat4>& world) {
    std::vector<glm::mat4> out(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        out[i] = world[i] * skeleton.joints[i].inverse_bind;
    }
    return out;
}

} // namespace

std::vector<glm::mat4> bind_skinning_matrices(const SkeletonAsset& skeleton) {
    return skinning_from_world(skeleton, compute_bind_world(skeleton));
}

std::vector<glm::mat4> sample_skinning_matrices(const SkeletonAsset& skeleton,
                                                const AnimClipAsset& clip, float time) {
    const std::vector<glm::mat4> local = sample_local_matrices(skeleton, clip, time);
    return skinning_from_world(skeleton, compute_world_matrices(skeleton, local));
}

std::expected<void, core::Error> run_animator_self_test() {
    // root (node 0) at origin; child (node 1) translated +2 X, parented to root.
    SkeletonAsset skel;
    skel.joints.resize(2);
    skel.joints[0].name = "root";
    skel.joints[0].parent = -1;
    skel.joints[0].node = 0;
    skel.joints[1].name = "child";
    skel.joints[1].parent = 0;
    skel.joints[1].node = 1;
    skel.joints[1].translation = {2.0F, 0.0F, 0.0F};

    // inverse bind = inverse of bind-pose world, so the bind pose skins to identity.
    const std::vector<glm::mat4> bind_world = compute_bind_world(skel);
    for (std::size_t i = 0; i < skel.joints.size(); ++i) {
        skel.joints[i].inverse_bind = glm::inverse(bind_world[i]);
    }

    // Clip translating the child: (2,0,0) at t=0 (= bind) -> (5,0,0) at t=1.
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler t_sampler;
    t_sampler.input = {0.0F, 1.0F};
    t_sampler.output = {{2, 0, 0, 0}, {5, 0, 0, 0}};
    clip.samplers = {t_sampler};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};

    // At the bind time the skinning matrices are identity.
    const std::vector<glm::mat4> m0 = sample_skinning_matrices(skel, clip, 0.0F);
    for (std::size_t i = 0; i < m0.size(); ++i) {
        if (glm::distance(m0[i][3], glm::vec4(0, 0, 0, 1)) > 1e-5F) {
            return std::unexpected(core::Error{"animator self-test: bind-time skinning not identity"});
        }
    }

    // At t=1 the child moved to world (5,0,0); skinning displaces a bind vertex by
    // (5-2, 0, 0) = (3,0,0). The root (unanimated) stays identity.
    const std::vector<glm::mat4> m1 = sample_skinning_matrices(skel, clip, 1.0F);
    if (glm::distance(glm::vec3(m1[1][3]), glm::vec3(3.0F, 0.0F, 0.0F)) > 1e-5F) {
        return std::unexpected(core::Error{"animator self-test: posed child displacement wrong"});
    }
    if (glm::distance(m1[0][3], glm::vec4(0, 0, 0, 1)) > 1e-5F) {
        return std::unexpected(core::Error{"animator self-test: unanimated root not identity"});
    }

    // bind_skinning_matrices agrees with sampling at the bind time.
    const std::vector<glm::mat4> b = bind_skinning_matrices(skel);
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (glm::distance(b[i][3], glm::vec4(0, 0, 0, 1)) > 1e-5F) {
            return std::unexpected(core::Error{"animator self-test: bind skinning not identity"});
        }
    }
    return {};
}

} // namespace engine::animation
