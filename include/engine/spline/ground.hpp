#pragma once

// Ground projection of curves (ROADMAP G18.1). The ground is sampled through
// a GroundHeightFn — the same sampler Scatter and navigation use
// (engine/scene/navigation.hpp declares the identical type), so streamed
// terrain tiles work: a point without loaded ground reports nullopt.

#include <cstddef>
#include <functional>
#include <optional>
#include <span>

#include "engine/spline/curve.hpp"
#include "engine/spline/spline.hpp"

namespace engine::spline {

using GroundHeightFn = std::function<std::optional<float>(float x, float z)>;

// Sets each sample's y to ground + offset and tilts its tangent to the
// draped slope (estimated from neighbouring samples, so pass them in curve
// order). Samples over missing ground keep their y. Returns how many had no
// ground (0 = fully draped).
std::size_t drape(std::span<CurveSample> samples, const GroundHeightFn& ground, float offset);

// Moves every point of `spline` onto the ground (+ offset), e.g. a stroke
// fitted in the XZ plane. Returns how many points had no ground.
std::size_t project_points(Spline& spline, const GroundHeightFn& ground, float offset);

} // namespace engine::spline
