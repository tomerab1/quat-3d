// glTF load-time clip compression across several skins: a body skin with
// every joint and a hair / clothing skin over a subset share one clip.

#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include "engine/animation/animator.hpp"
#include "engine/animation/compression.hpp"
#include "scene/gltf/clip_compression.hpp"

namespace {

namespace anim = engine::animation;

// A chain of `nodes` (glTF node ids), 0.2 m apart along +Y.
[[nodiscard]] anim::SkeletonAsset chain(std::vector<int> nodes) {
    anim::SkeletonAsset skel;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        anim::Joint j;
        j.name = "n" + std::to_string(nodes[i]);
        j.node = nodes[i];
        j.parent = static_cast<int>(i) - 1;
        j.translation = i == 0 ? glm::vec3(0.0F) : glm::vec3(0.0F, 0.2F, 0.0F);
        skel.joints.push_back(j);
    }
    return skel;
}

// Every node of the body bends about Z, 1 s at 30 Hz.
[[nodiscard]] anim::AnimClipAsset bend_clip(const std::vector<int>& nodes) {
    anim::AnimClipAsset clip;
    clip.name = "bend";
    clip.duration = 1.0F;
    for (const int node : nodes) {
        anim::AnimSampler s;
        for (int k = 0; k <= 30; ++k) {
            const float t = static_cast<float>(k) / 30.0F;
            const glm::quat q = glm::angleAxis(0.4F * std::sin(4.0F * t + node), glm::vec3(0, 0, 1));
            s.input.push_back(t);
            s.output.emplace_back(q.x, q.y, q.z, q.w);
        }
        clip.samplers.push_back(s);
        clip.channels.push_back({node, anim::AnimPath::rotation, clip.samplers.size() - 1});
    }
    return clip;
}

TEST(ClipCompression, ASubsetSkinPlaysTheClipCompressedForTheBody) {
    const std::vector<int> body_nodes{0, 1, 2, 3, 4, 5, 6, 7};
    const std::vector<anim::SkeletonAsset> skins{chain(body_nodes), chain({5, 6, 7})};
    std::vector<anim::AnimClipAsset> clips{bend_clip(body_nodes)};
    const anim::AnimClipAsset raw = clips[0];

    engine::scene::compress_clips(clips, skins, {glm::mat4(1.0F), glm::mat4(1.0F)}, "test");
    ASSERT_NE(clips[0].compressed, nullptr);
    EXPECT_TRUE(clips[0].samplers.empty()) << "raw keys dropped: every channel is a body joint";

    const anim::SkeletonAsset& hair = skins[1];
    const std::vector<anim::JointPose> bind = anim::bind_pose(hair);
    const std::vector<anim::JointPose> expected = anim::sample_local_pose(hair, raw, 0.45F);
    const std::vector<anim::JointPose> pose = anim::sample_local_pose(hair, clips[0], 0.45F);
    for (std::size_t j = 0; j < hair.joints.size(); ++j) {
        EXPECT_GT(std::abs(glm::dot(pose[j].rotation, bind[j].rotation) - 1.0F), 1e-6F)
            << "hair joint " << j << " plays the bind pose";
        EXPECT_GT(std::abs(glm::dot(pose[j].rotation, expected[j].rotation)), 1.0F - 1e-6F)
            << "hair joint " << j << " differs from the raw clip";
    }
}

TEST(ClipCompression, ClipsDrivingNodesOutsideTheSkinKeepRawKeys) {
    // The clip also animates node 9, which only the prop skin has.
    const std::vector<anim::SkeletonAsset> skins{chain({0, 1, 2, 3}), chain({9})};
    std::vector<anim::AnimClipAsset> clips{bend_clip({0, 1, 2, 3, 9})};
    engine::scene::compress_clips(clips, skins, {glm::mat4(1.0F), glm::mat4(1.0F)}, "test");
    ASSERT_NE(clips[0].compressed, nullptr);
    EXPECT_FALSE(clips[0].samplers.empty());
    const std::vector<anim::JointPose> prop = anim::sample_local_pose(skins[1], clips[0], 0.3F);
    EXPECT_GT(std::abs(prop[0].rotation.z), 1e-3F) << "the prop joint must still animate";
}

} // namespace
