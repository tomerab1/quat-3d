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

std::expected<void, core::Error> run_clip_self_test() {
    AnimClipAsset clip;
    clip.name = "test";
    clip.duration = 1.0F;

    // sampler 0: translation, LINEAR, (0,0,0) -> (10,0,0) over [0,1].
    AnimSampler t_sampler;
    t_sampler.input = {0.0F, 1.0F};
    t_sampler.output = {{0, 0, 0, 0}, {10, 0, 0, 0}};
    t_sampler.interpolation = Interpolation::linear;

    // sampler 1: scale, STEP, (1,1,1) then (2,2,2).
    AnimSampler s_sampler;
    s_sampler.input = {0.0F, 1.0F};
    s_sampler.output = {{1, 1, 1, 0}, {2, 2, 2, 0}};
    s_sampler.interpolation = Interpolation::step;

    // sampler 2: rotation, LINEAR (slerp), identity -> 90 deg about Y.
    AnimSampler r_sampler;
    r_sampler.input = {0.0F, 1.0F};
    r_sampler.output = {{0, 0, 0, 1}, {0, 0.70710678F, 0, 0.70710678F}};
    r_sampler.interpolation = Interpolation::linear;

    clip.samplers = {t_sampler, s_sampler, r_sampler};

    // Translation: linear midpoint and clamping past the ends.
    if (glm::distance(t_sampler.sample_vec3(0.5F), glm::vec3(5, 0, 0)) > 1e-5F) {
        return std::unexpected(core::Error{"clip self-test: linear translation midpoint wrong"});
    }
    if (glm::distance(t_sampler.sample_vec3(-1.0F), glm::vec3(0, 0, 0)) > 1e-5F ||
        glm::distance(t_sampler.sample_vec3(5.0F), glm::vec3(10, 0, 0)) > 1e-5F) {
        return std::unexpected(core::Error{"clip self-test: translation clamping wrong"});
    }

    // Scale STEP: holds the previous key until the next time is reached.
    if (glm::distance(s_sampler.sample_vec3(0.5F), glm::vec3(1, 1, 1)) > 1e-5F ||
        glm::distance(s_sampler.sample_vec3(1.0F), glm::vec3(2, 2, 2)) > 1e-5F) {
        return std::unexpected(core::Error{"clip self-test: STEP scale wrong"});
    }

    // Rotation slerp: halfway is 45 deg about Y.
    const glm::quat mid = r_sampler.sample_quat(0.5F);
    const glm::quat expected(0.92387953F, 0.0F, 0.38268343F, 0.0F); // (w,x,y,z), 45 deg Y
    if (glm::abs(glm::dot(mid, expected)) < 0.9999F) {
        return std::unexpected(core::Error{"clip self-test: rotation slerp midpoint wrong"});
    }

    // Malformed sampler (S1 audit B11): more input keys than output values
    // must clamp to the last value instead of reading past the array.
    AnimSampler short_sampler;
    short_sampler.input = {0.0F, 1.0F, 2.0F, 3.0F};
    short_sampler.output = {{1, 2, 3, 0}, {4, 5, 6, 0}};
    short_sampler.interpolation = Interpolation::linear;
    if (glm::distance(short_sampler.sample_vec3(2.5F), glm::vec3(4, 5, 6)) > 1e-5F ||
        glm::distance(short_sampler.sample_vec3(9.0F), glm::vec3(4, 5, 6)) > 1e-5F) {
        return std::unexpected(core::Error{"clip self-test: short output not clamped"});
    }
    short_sampler.output = {{0, 0, 0, 1}};
    if (glm::abs(glm::dot(short_sampler.sample_quat(2.5F), glm::quat(1, 0, 0, 0))) < 0.9999F) {
        return std::unexpected(core::Error{"clip self-test: short quat output not clamped"});
    }

    return {};
}

} // namespace engine::animation
