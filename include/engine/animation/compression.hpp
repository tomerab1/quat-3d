#pragma once

// Animation compression with ACL (Frechette's Animation Compression Library,
// MIT; Unreal's default animation codec since 5.3).
//
// A clip is resampled uniformly at its key rate into one transform track per
// skeleton joint and compressed with ACL's variable bit rates, bounded by an
// object-space error measured on a shell of virtual vertices around each
// joint. Sampling decompresses straight into the local pose. The ACL headers
// stay inside the codec's translation units.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/core/error.hpp"

namespace engine::animation {

struct CompressionSettings {
    // Largest allowed object-space error of a virtual vertex `shell_distance`
    // from any joint (metres). 0.1 mm / 3 cm match ACL's defaults in metres.
    float precision = 0.0001F;
    float shell_distance = 0.03F;
    // Resampling rate bounds (Hz). Clips resample at a whole multiple of
    // their key rate, raised until the interpolation between samples also
    // meets `precision`.
    float min_sample_rate = 24.0F;
    float max_sample_rate = 120.0F;
    // Let ACL drop samples it can rebuild within `precision`. Smaller, but
    // the bound then holds only at the kept samples, not between them.
    bool strip_keyframes = false;
    // Keep the raw keys after compressing (only needed to play the clip on a
    // different skeleton). Clips whose channels drive nodes outside the
    // skeleton always keep them.
    bool keep_raw = false;
};

struct CompressionStats {
    std::size_t   raw_bytes = 0;        // keyframe payload before
    std::size_t   compressed_bytes = 0; // ACL buffer + binding after
    float         sample_rate = 0.0F;   // Hz the clip was resampled at
    std::uint32_t samples = 0;          // per track
    bool          raw_dropped = false;
};

// An ACL-compressed clip bound to one skeleton layout (one track per joint).
struct CompressedClip {
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    std::vector<Block> buffer;      // acl::compressed_tracks, 16-byte aligned
    std::size_t        size = 0;    // bytes used in `buffer`
    std::vector<int>   joint_nodes; // glTF node of each track's joint
    float              duration = 0.0F;

    [[nodiscard]] std::size_t size_bytes() const noexcept {
        return size + joint_nodes.size() * sizeof(int);
    }
    // Same joint count and node per joint as the skeleton it was built for.
    [[nodiscard]] bool binds(const SkeletonAsset& skeleton) const noexcept;
};

// Keyframe payload of the clip's raw samplers (times + values).
[[nodiscard]] std::size_t raw_clip_bytes(const AnimClipAsset& clip);

// Compresses `clip` for `skeleton` and attaches the result (clip.compressed);
// sample_local_pose then decompresses whenever the skeleton matches.
// `metres_per_unit` is the skeleton's scale in the world (its armature
// transform), so the error bounds stay in metres. Refused (the clip stays
// raw) for STEP keys, motion too fast for max_sample_rate, or no saving.
[[nodiscard]] std::expected<CompressionStats, core::Error>
compress_clip(AnimClipAsset& clip, const SkeletonAsset& skeleton,
              const CompressionSettings& settings = {}, float metres_per_unit = 1.0F);

// Decompresses the pose at `time` (clamped to the clip) into `pose`, one
// entry per track. `pose` must hold the skeleton's bind pose on entry:
// sub-tracks equal to it are not stored and are left untouched.
void decompress_pose(const CompressedClip& compressed, float time, std::span<JointPose> pose);

} // namespace engine::animation
