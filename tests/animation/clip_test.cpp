// Animation clip sampling: LINEAR / STEP / slerp, clamping, malformed samplers.

#include <gtest/gtest.h>

#include "engine/animation/clip.hpp"
#include "support/glm_near.hpp"

namespace {

using engine::animation::AnimSampler;
using engine::animation::Interpolation;

[[nodiscard]] AnimSampler two_key_sampler(glm::vec4 first, glm::vec4 last, Interpolation interp) {
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {first, last};
    s.interpolation = interp;
    return s;
}

TEST(AnimClip, LinearTranslationInterpolates) {
    const AnimSampler s =
        two_key_sampler({0, 0, 0, 0}, {10, 0, 0, 0}, Interpolation::linear);
    EXPECT_VEC_NEAR(s.sample_vec3(0.5F), glm::vec3(5, 0, 0), 1e-5F);
}

TEST(AnimClip, SamplingClampsPastBothEnds) {
    const AnimSampler s =
        two_key_sampler({0, 0, 0, 0}, {10, 0, 0, 0}, Interpolation::linear);
    EXPECT_VEC_NEAR(s.sample_vec3(-1.0F), glm::vec3(0, 0, 0), 1e-5F);
    EXPECT_VEC_NEAR(s.sample_vec3(5.0F), glm::vec3(10, 0, 0), 1e-5F);
}

TEST(AnimClip, StepHoldsThePreviousKeyUntilTheNext) {
    const AnimSampler s = two_key_sampler({1, 1, 1, 0}, {2, 2, 2, 0}, Interpolation::step);
    EXPECT_VEC_NEAR(s.sample_vec3(0.5F), glm::vec3(1, 1, 1), 1e-5F);
    EXPECT_VEC_NEAR(s.sample_vec3(1.0F), glm::vec3(2, 2, 2), 1e-5F);
}

TEST(AnimClip, RotationSlerpsInGltfXyzwOrder) {
    // identity -> 90 deg about Y; halfway is 45 deg about Y.
    const AnimSampler s = two_key_sampler({0, 0, 0, 1}, {0, 0.70710678F, 0, 0.70710678F},
                                          Interpolation::linear);
    const glm::quat expected(0.92387953F, 0.0F, 0.38268343F, 0.0F); // (w,x,y,z)
    EXPECT_SAME_ROTATION(s.sample_quat(0.5F), expected, 1e-4F);
}

// S1 audit B11: more input keys than output values must clamp to the last
// value instead of reading past the array.
TEST(AnimClip, ShortOutputClampsToTheLastValue) {
    AnimSampler s;
    s.input = {0.0F, 1.0F, 2.0F, 3.0F};
    s.output = {{1, 2, 3, 0}, {4, 5, 6, 0}};
    s.interpolation = Interpolation::linear;
    EXPECT_VEC_NEAR(s.sample_vec3(2.5F), glm::vec3(4, 5, 6), 1e-5F);
    EXPECT_VEC_NEAR(s.sample_vec3(9.0F), glm::vec3(4, 5, 6), 1e-5F);

    s.output = {{0, 0, 0, 1}};
    EXPECT_SAME_ROTATION(s.sample_quat(2.5F), glm::quat(1, 0, 0, 0), 1e-4F);
}

} // namespace
