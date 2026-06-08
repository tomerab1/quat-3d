#pragma once

// AnimClipAsset — a sampled animation clip (Phase 5, Slice 5.2).
//
// A glTF animation flattened into samplers (keyframe times + values) and channels
// (which node/path each sampler drives). Supports LINEAR and STEP interpolation;
// CUBICSPLINE is approximated as LINEAR over its value keyframes (proper cubic is
// a Phase 11 stretch). The animator (5.3) samples each channel at the current
// time and maps the target node onto a skeleton joint.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/error.hpp"

namespace engine::animation {

// Which component of a joint's local transform a channel drives.
enum class AnimPath : std::uint8_t { translation, rotation, scale };

// Keyframe interpolation. CUBICSPLINE is collapsed to `linear` at load time.
enum class Interpolation : std::uint8_t { linear, step };

// Keyframe times + values for one animated quantity. `output` holds a vec3 in
// xyz (translation/scale, w unused) or a quaternion as (x, y, z, w) (rotation),
// one per input time.
struct AnimSampler {
    std::vector<float>     input;  // ascending keyframe times (seconds)
    std::vector<glm::vec4> output; // one value per input time
    Interpolation          interpolation = Interpolation::linear;

    // Sample at time `t` (clamped to the keyframe range).
    [[nodiscard]] glm::vec3 sample_vec3(float t) const;
    [[nodiscard]] glm::quat sample_quat(float t) const;
};

// Binds a sampler to the node/path it animates.
struct AnimChannel {
    int         target_node = -1;
    AnimPath    path = AnimPath::translation;
    std::size_t sampler = 0; // index into AnimClipAsset::samplers
};

struct AnimClipAsset {
    std::string              name;
    float                    duration = 0.0F; // max keyframe time across samplers
    std::vector<AnimSampler> samplers;
    std::vector<AnimChannel> channels;

    [[nodiscard]] bool valid() const noexcept { return !channels.empty(); }
};

// Builds a clip in code (translation LINEAR, scale STEP, rotation slerp) and
// verifies sampling at several times, including clamping past the ends.
[[nodiscard]] std::expected<void, core::Error> run_clip_self_test();

} // namespace engine::animation
