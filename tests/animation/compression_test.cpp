// ACL compression: decompressed poses stay within the configured error of
// the raw clip, memory shrinks, and the skeleton binding is respected.

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include "engine/animation/compression.hpp"
#include "support/expect_ok.hpp"

namespace {

using namespace engine::animation;

// A 1 + 4x5 joint tree (root with four 5-joint limbs, 0.15 m bones).
[[nodiscard]] SkeletonAsset tree_skeleton() {
    SkeletonAsset skel;
    Joint root;
    root.name = "root";
    root.node = 0;
    skel.joints.push_back(root);
    const glm::vec3 dirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    for (int limb = 0; limb < 4; ++limb) {
        int parent = 0;
        for (int k = 0; k < 5; ++k) {
            Joint j;
            j.name = "j" + std::to_string(limb) + "_" + std::to_string(k);
            j.parent = parent;
            j.node = static_cast<int>(skel.joints.size());
            j.translation = dirs[limb] * 0.15F;
            parent = j.node;
            skel.joints.push_back(j);
        }
    }
    return skel;
}

// 2 s at 30 Hz: every joint swings on its own sine, the root also travels.
[[nodiscard]] AnimClipAsset swing_clip(const SkeletonAsset& skel) {
    AnimClipAsset clip;
    clip.name = "swing";
    clip.duration = 2.0F;
    constexpr int keys = 61;
    for (std::size_t j = 0; j < skel.joints.size(); ++j) {
        AnimSampler rot;
        for (int k = 0; k < keys; ++k) {
            const float t = static_cast<float>(k) / 30.0F;
            const float phase = static_cast<float>(j) * 0.7F;
            const glm::quat q =
                glm::angleAxis(0.6F * std::sin(3.0F * t + phase), glm::vec3(0, 0, 1)) *
                glm::angleAxis(0.3F * std::cos(2.0F * t + phase), glm::vec3(1, 0, 0));
            rot.input.push_back(t);
            rot.output.emplace_back(q.x, q.y, q.z, q.w);
        }
        clip.samplers.push_back(rot);
        clip.channels.push_back({skel.joints[j].node, AnimPath::rotation, clip.samplers.size() - 1});
    }
    AnimSampler move;
    for (int k = 0; k < keys; ++k) {
        const float t = static_cast<float>(k) / 30.0F;
        move.input.push_back(t);
        move.output.emplace_back(0.8F * t, 0.05F * std::sin(6.0F * t), 0.0F, 0.0F);
    }
    clip.samplers.push_back(move);
    clip.channels.push_back({0, AnimPath::translation, clip.samplers.size() - 1});
    return clip;
}

// Largest distance between matching virtual vertices (6 per joint, at
// `shell` along the joint's axes) of two poses, in object space.
[[nodiscard]] float shell_error(const SkeletonAsset& skel, const std::vector<JointPose>& a,
                                const std::vector<JointPose>& b, float shell) {
    const std::vector<glm::mat4> wa = pose_world_matrices(skel, a);
    const std::vector<glm::mat4> wb = pose_world_matrices(skel, b);
    float worst = 0.0F;
    for (std::size_t j = 0; j < wa.size(); ++j) {
        for (int axis = 0; axis < 3; ++axis) {
            for (const float sign : {-1.0F, 1.0F}) {
                glm::vec4 p(0.0F, 0.0F, 0.0F, 1.0F);
                p[axis] = sign * shell;
                worst = std::max(worst, glm::length(glm::vec3(wa[j] * p) - glm::vec3(wb[j] * p)));
            }
        }
    }
    return worst;
}

TEST(AclCompression, DecompressedPosesStayWithinTheConfiguredError) {
    const SkeletonAsset skel = tree_skeleton();
    const AnimClipAsset raw = swing_clip(skel);
    AnimClipAsset clip = raw;
    const CompressionSettings settings; // 0.1 mm at 3 cm
    const auto stats = compress_clip(clip, skel, settings);
    ASSERT_OK(stats);
    ASSERT_NE(clip.compressed, nullptr);
    EXPECT_TRUE(clip.valid());
    EXPECT_EQ(stats->samples, 61U);
    EXPECT_FLOAT_EQ(stats->sample_rate, 30.0F);

    // On the keys the raw and resampled clips agree, so ACL's bound applies
    // directly; between keys the interpolation schemes differ slightly.
    float worst_key = 0.0F;
    float worst_between = 0.0F;
    for (int k = 0; k <= 120; ++k) {
        const float t = static_cast<float>(k) / 60.0F;
        const float e = shell_error(skel, sample_local_pose(skel, raw, t),
                                    sample_local_pose(skel, clip, t), settings.shell_distance);
        (k % 2 == 0 ? worst_key : worst_between) = std::max(k % 2 == 0 ? worst_key : worst_between, e);
    }
    EXPECT_LE(worst_key, settings.precision * 1.5F) << "error on the keys";
    EXPECT_LE(worst_between, settings.precision * 5.0F) << "error between keys";
}

TEST(AclCompression, MemoryShrinksAndRawKeysAreDropped) {
    const SkeletonAsset skel = tree_skeleton();
    AnimClipAsset clip = swing_clip(skel);
    const std::size_t raw_bytes = raw_clip_bytes(clip);
    const auto stats = compress_clip(clip, skel);
    ASSERT_OK(stats);
    EXPECT_EQ(stats->raw_bytes, raw_bytes);
    EXPECT_TRUE(stats->raw_dropped);
    EXPECT_TRUE(clip.samplers.empty());
    EXPECT_LT(stats->compressed_bytes * 3, stats->raw_bytes)
        << stats->raw_bytes << " -> " << stats->compressed_bytes << " bytes";
    EXPECT_FALSE(compress_clip(clip, skel).has_value()) << "compressing twice";
}

TEST(AclCompression, RawKeysStayWhenAskedOrWhenTheClipDrivesOtherNodes) {
    const SkeletonAsset skel = tree_skeleton();
    AnimClipAsset keep = swing_clip(skel);
    CompressionSettings settings;
    settings.keep_raw = true;
    ASSERT_OK(compress_clip(keep, skel, settings));
    EXPECT_FALSE(keep.samplers.empty());

    AnimClipAsset extra = swing_clip(skel);
    extra.channels.push_back({99, AnimPath::translation, 0}); // a node outside the skin
    const auto stats = compress_clip(extra, skel);
    ASSERT_OK(stats);
    EXPECT_FALSE(stats->raw_dropped);
}

TEST(AclCompression, OnlyTheBoundSkeletonDecompresses) {
    const SkeletonAsset skel = tree_skeleton();
    AnimClipAsset clip = swing_clip(skel);
    ASSERT_OK(compress_clip(clip, skel));
    EXPECT_TRUE(clip.compressed->binds(skel));

    SkeletonAsset other = skel;
    other.joints[3].node = 42;
    EXPECT_FALSE(clip.compressed->binds(other));
    // Raw keys are gone: another layout gets its bind pose, never garbage.
    const std::vector<JointPose> pose = sample_local_pose(other, clip, 0.5F);
    const std::vector<JointPose> bind = bind_pose(other);
    EXPECT_EQ(pose[1].translation, bind[1].translation);
    EXPECT_EQ(pose[1].rotation, bind[1].rotation);
}

TEST(AclCompression, RejectsUnusableInput) {
    const SkeletonAsset skel = tree_skeleton();
    AnimClipAsset empty;
    EXPECT_FALSE(compress_clip(empty, skel).has_value());
    AnimClipAsset no_time = swing_clip(skel);
    no_time.duration = 0.0F;
    EXPECT_FALSE(compress_clip(no_time, skel).has_value());
}

} // namespace
