// Animation clip sampling (Phase 5, Slice 5.2).

#include "engine/animation/clip.hpp"

#include <glm/gtc/quaternion.hpp>

namespace engine::animation {

namespace {

// Locate the keyframe segment containing `t`: indices i0/i1 and the in-segment
// fraction f in [0,1]. Clamps to the first/last key outside the range.
struct Segment {
    std::size_t i0 = 0;
    std::size_t i1 = 0;
    float       f = 0.0F;
};

[[nodiscard]] Segment locate(const std::vector<float>& input, float t) {
    if (input.size() <= 1 || t <= input.front()) {
        return {0, 0, 0.0F};
    }
    if (t >= input.back()) {
        const std::size_t last = input.size() - 1;
        return {last, last, 0.0F};
    }
    std::size_t i1 = 1;
    while (i1 < input.size() && input[i1] <= t) {
        ++i1;
    }
    const std::size_t i0 = i1 - 1;
    const float span = input[i1] - input[i0];
    const float f = span > 0.0F ? (t - input[i0]) / span : 0.0F;
    return {i0, i1, f};
}

// Keyframe indices come from `input`; a malformed asset can carry fewer
// output values than input keys, so clamp them to the output array.
[[nodiscard]] Segment clamp_to_output(Segment s, std::size_t output_size) {
    const std::size_t last = output_size - 1;
    if (s.i0 > last) s.i0 = last;
    if (s.i1 > last) s.i1 = last;
    return s;
}

} // namespace

glm::vec3 AnimSampler::sample_vec3(float t) const {
    if (output.empty()) return glm::vec3(0.0F);
    const Segment s = clamp_to_output(locate(input, t), output.size());
    const glm::vec3 a(output[s.i0]);
    if (interpolation == Interpolation::step || s.i0 == s.i1) {
        return a;
    }
    return glm::mix(a, glm::vec3(output[s.i1]), s.f);
}

glm::quat AnimSampler::sample_quat(float t) const {
    if (output.empty()) return glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    const Segment s = clamp_to_output(locate(input, t), output.size());
    // glTF stores quaternions as (x, y, z, w); glm::quat ctor is (w, x, y, z).
    const glm::quat a(output[s.i0].w, output[s.i0].x, output[s.i0].y, output[s.i0].z);
    if (interpolation == Interpolation::step || s.i0 == s.i1) {
        return glm::normalize(a);
    }
    const glm::quat b(output[s.i1].w, output[s.i1].x, output[s.i1].y, output[s.i1].z);
    return glm::normalize(glm::slerp(a, b, s.f));
}

} // namespace engine::animation
