// ACL compression of glTF clips (one qvv track per skeleton joint).

#include "engine/animation/compression.hpp"

#include "clip_resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>

namespace engine::animation {

namespace {

[[nodiscard]] rtm::qvvf to_qvv(const JointPose& p) {
    const glm::quat q = glm::normalize(p.rotation);
    return rtm::qvv_set(rtm::quat_set(q.x, q.y, q.z, q.w),
                        rtm::vector_set(p.translation.x, p.translation.y, p.translation.z),
                        rtm::vector_set(p.scale.x, p.scale.y, p.scale.z));
}

// Every channel drives a joint of `skeleton` (the raw keys are redundant).
[[nodiscard]] bool covered_by(const AnimClipAsset& clip, const SkeletonAsset& skeleton) {
    for (const AnimChannel& ch : clip.channels) {
        const bool found = std::any_of(skeleton.joints.begin(), skeleton.joints.end(),
                                       [&](const Joint& j) { return j.node == ch.target_node; });
        if (!found) return false;
    }
    return true;
}

} // namespace

int CompressedClip::track_of(int node) const noexcept {
    const auto it = std::lower_bound(node_tracks.begin(), node_tracks.end(),
                                     std::pair<int, std::uint32_t>{node, 0U});
    return it != node_tracks.end() && it->first == node ? static_cast<int>(it->second) : -1;
}

bool CompressedClip::covers(const SkeletonAsset& skeleton) const noexcept {
    return std::all_of(skeleton.joints.begin(), skeleton.joints.end(),
                       [&](const Joint& j) { return track_of(j.node) >= 0; });
}

bool CompressedClip::binds(const SkeletonAsset& skeleton) const noexcept {
    if (joint_nodes.size() != skeleton.joints.size()) return false;
    for (std::size_t i = 0; i < joint_nodes.size(); ++i) {
        if (joint_nodes[i] != skeleton.joints[i].node) return false;
    }
    return true;
}

std::size_t raw_clip_bytes(const AnimClipAsset& clip) {
    std::size_t bytes = clip.channels.size() * sizeof(AnimChannel);
    for (const AnimSampler& s : clip.samplers) {
        bytes += s.input.size() * sizeof(float) + s.output.size() * sizeof(glm::vec4);
    }
    return bytes;
}

std::expected<CompressionStats, core::Error> compress_clip(AnimClipAsset& clip,
                                                           const SkeletonAsset& skeleton,
                                                           const CompressionSettings& settings,
                                                           float metres_per_unit) {
    const auto fail = [](std::string what) {
        return std::unexpected(core::Error{"compress_clip: " + std::move(what)});
    };
    const std::size_t joints = skeleton.joints.size();
    if (clip.compressed != nullptr) return fail("clip '" + clip.name + "' is already compressed");
    if (joints == 0 || clip.channels.empty()) return fail("empty skeleton or clip");
    if (!(clip.duration > 0.0F) || !std::isfinite(clip.duration)) {
        return fail("clip '" + clip.name + "' has no duration");
    }
    if (!(settings.precision > 0.0F) || !(settings.shell_distance > 0.0F) ||
        !(metres_per_unit > 0.0F) || !std::isfinite(metres_per_unit)) {
        return fail("precision, shell distance and unit scale must be positive");
    }
    for (const AnimSampler& s : clip.samplers) {
        // ACL interpolates every track; stepped keys would blur between samples.
        if (s.interpolation == Interpolation::step && s.output.size() > 1) {
            return fail("clip '" + clip.name + "' has STEP keys (kept uncompressed)");
        }
    }
    for (const Joint& j : skeleton.joints) {
        if (j.parent >= static_cast<int>(joints)) return fail("joint parent out of range");
    }

    CompressionStats stats;
    stats.raw_bytes = raw_clip_bytes(clip);
    const float precision = settings.precision / metres_per_unit; // skeleton units
    const float shell = settings.shell_distance / metres_per_unit;
    // Half the budget for interpolation between samples, half for ACL.
    const float rate = plan_sample_rate(clip, skeleton, settings.min_sample_rate,
                                        settings.max_sample_rate, 0.5F * precision, shell);
    if (!(rate > 0.0F)) {
        return fail("clip '" + clip.name + "' rotates too fast between keys for " +
                    std::to_string(static_cast<int>(settings.max_sample_rate)) + " Hz at this precision (kept raw)");
    }
    const auto samples = static_cast<std::uint32_t>(
        std::max(2.0F, std::ceil(clip.duration * rate - 1e-3F) + 1.0F));
    const float sample_rate = static_cast<float>(samples - 1) / clip.duration;
    stats.sample_rate = sample_rate;
    stats.samples = samples;

    // Raw tracks: the clip sampled uniformly, one qvv track per joint whose
    // default value is the bind pose (bind-pose sub-tracks cost nothing).
    acl::ansi_allocator allocator;
    acl::track_array_qvvf raw(allocator, static_cast<std::uint32_t>(joints));
    const std::vector<JointPose> bind = bind_pose(skeleton);
    for (std::size_t j = 0; j < joints; ++j) {
        acl::track_desc_transformf desc;
        desc.output_index = static_cast<std::uint32_t>(j);
        const int parent = skeleton.joints[j].parent;
        desc.parent_index =
            parent >= 0 ? static_cast<std::uint32_t>(parent) : acl::k_invalid_track_index;
        desc.precision = 0.5F * precision; // the other half: interpolation
        desc.shell_distance = shell;
        desc.default_value = to_qvv(bind[j]);
        raw[static_cast<std::uint32_t>(j)] =
            acl::track_qvvf::make_reserve(desc, allocator, samples, sample_rate);
    }
    for (std::uint32_t s = 0; s < samples; ++s) {
        const float t = std::min(static_cast<float>(s) / sample_rate, clip.duration);
        const std::vector<JointPose> pose = sample_local_pose(skeleton, clip, t);
        for (std::size_t j = 0; j < joints; ++j) {
            raw[static_cast<std::uint32_t>(j)][s] = to_qvv(pose[j]);
        }
    }

    acl::compression_settings acl_settings = acl::get_default_compression_settings();
    acl::qvvf_matrix3x4f_transform_error_metric error_metric; // matches glm matrix skinning
    acl_settings.error_metric = &error_metric;
    // Stripping is checked only at the removed samples, so it would void the
    // between-sample half of the budget; opt-in.
    acl_settings.keyframe_stripping.strip_trivial = settings.strip_keyframes;

    acl::output_stats acl_stats;
    acl::compressed_tracks* tracks = nullptr;
    const acl::error_result result =
        acl::compress_track_list(allocator, raw, acl_settings, tracks, acl_stats);
    if (result.any() || tracks == nullptr) {
        return fail("clip '" + clip.name + "': " + (result.any() ? result.c_str() : "no output"));
    }

    auto out = std::make_shared<CompressedClip>();
    out->size = tracks->get_size();
    if (out->size + joints * sizeof(int) >= stats.raw_bytes) {
        allocator.deallocate(tracks, out->size);
        return fail("clip '" + clip.name + "' does not shrink (" + std::to_string(out->size) +
                    " B compressed vs " + std::to_string(stats.raw_bytes) + " B raw; kept raw)");
    }
    out->buffer.resize((out->size + sizeof(CompressedClip::Block) - 1) /
                       sizeof(CompressedClip::Block));
    std::memcpy(out->buffer.data(), tracks, out->size);
    allocator.deallocate(tracks, out->size);
    out->duration = clip.duration;
    out->joint_nodes.reserve(joints);
    out->node_tracks.reserve(joints);
    for (std::size_t j = 0; j < joints; ++j) {
        out->joint_nodes.push_back(skeleton.joints[j].node);
        out->node_tracks.emplace_back(skeleton.joints[j].node, static_cast<std::uint32_t>(j));
    }
    std::sort(out->node_tracks.begin(), out->node_tracks.end());
    stats.compressed_bytes = out->size_bytes();

    if (!settings.keep_raw && covered_by(clip, skeleton)) {
        clip.samplers = {};
        clip.channels = {};
        stats.raw_dropped = true;
    }
    clip.compressed = std::move(out);
    return stats;
}

} // namespace engine::animation
