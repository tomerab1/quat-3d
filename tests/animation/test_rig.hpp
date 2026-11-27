#pragma once

// Tiny rigs shared by the animation tests.

#include <vector>

#include <glm/glm.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"

namespace quat::test {

// Two joints: root (node 0) at the origin, child (node 1) at +2 X under it,
// with inverse_bind set so the bind pose skins to identity.
[[nodiscard]] inline engine::animation::SkeletonAsset two_joint_skeleton() {
    engine::animation::SkeletonAsset skel;
    skel.joints.resize(2);
    skel.joints[0].name = "root";
    skel.joints[0].parent = -1;
    skel.joints[0].node = 0;
    skel.joints[1].name = "child";
    skel.joints[1].parent = 0;
    skel.joints[1].node = 1;
    skel.joints[1].translation = {2.0F, 0.0F, 0.0F};
    const std::vector<glm::mat4> bw = engine::animation::compute_bind_world(skel);
    for (std::size_t i = 0; i < skel.joints.size(); ++i) {
        skel.joints[i].inverse_bind = glm::inverse(bw[i]);
    }
    return skel;
}

// A one-second clip holding the child (node 1) at a constant translation.
[[nodiscard]] inline engine::animation::AnimClipAsset child_translation_clip(glm::vec3 t) {
    using namespace engine::animation;
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {glm::vec4(t, 0.0F), glm::vec4(t, 0.0F)};
    clip.samplers = {s};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};
    return clip;
}

// Translation column of a skinning matrix: where it moves a vertex at the origin.
[[nodiscard]] inline glm::vec3 displacement(const glm::mat4& m) { return glm::vec3(m[3]); }

constexpr glm::vec4 k_no_translation(0.0F, 0.0F, 0.0F, 1.0F);

} // namespace quat::test
