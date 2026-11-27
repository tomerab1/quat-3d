// ACL decompression straight into a local pose.

#include "engine/animation/compression.hpp"

#include <algorithm>

#include <acl/core/compressed_tracks.h>
#include <acl/decompression/decompress.h>

namespace engine::animation {

namespace {

// Writes decompressed tracks into JointPose entries; bind-pose (default)
// sub-tracks are skipped, so the caller's pre-filled bind pose stays.
struct PoseWriter final : acl::track_writer {
    explicit PoseWriter(std::span<JointPose> out) : pose(out) {}

    static constexpr acl::default_sub_track_mode get_default_rotation_mode() {
        return acl::default_sub_track_mode::skipped;
    }
    static constexpr acl::default_sub_track_mode get_default_translation_mode() {
        return acl::default_sub_track_mode::skipped;
    }
    static constexpr acl::default_sub_track_mode get_default_scale_mode() {
        return acl::default_sub_track_mode::skipped;
    }

    void RTM_SIMD_CALL write_rotation(std::uint32_t track, rtm::quatf_arg0 q) {
        if (track < pose.size()) {
            pose[track].rotation = glm::quat(rtm::quat_get_w(q), rtm::quat_get_x(q),
                                             rtm::quat_get_y(q), rtm::quat_get_z(q));
        }
    }
    void RTM_SIMD_CALL write_translation(std::uint32_t track, rtm::vector4f_arg0 t) {
        if (track < pose.size()) {
            pose[track].translation =
                glm::vec3(rtm::vector_get_x(t), rtm::vector_get_y(t), rtm::vector_get_z(t));
        }
    }
    void RTM_SIMD_CALL write_scale(std::uint32_t track, rtm::vector4f_arg0 s) {
        if (track < pose.size()) {
            pose[track].scale =
                glm::vec3(rtm::vector_get_x(s), rtm::vector_get_y(s), rtm::vector_get_z(s));
        }
    }

    std::span<JointPose> pose;
};

} // namespace

void decompress_pose(const CompressedClip& compressed, float time, std::span<JointPose> pose) {
    if (compressed.size == 0) return;
    const auto* tracks = reinterpret_cast<const acl::compressed_tracks*>(compressed.buffer.data());
    acl::decompression_context<acl::default_transform_decompression_settings> context;
    if (!context.initialize(*tracks)) return;
    context.seek(std::clamp(time, 0.0F, compressed.duration), acl::sample_rounding_policy::none);
    PoseWriter writer(pose);
    context.decompress_tracks(writer);
}

} // namespace engine::animation
