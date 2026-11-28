#pragma once

// The `spline` component and its derived cache (ROADMAP G18.1).
//
// Spline (engine/spline/spline.hpp) is the authored data: entity-local
// control points with width / height / tags, the curve kind, closed,
// follow_ground, the curve network it joins. SplineCache is derived, owned by
// update_spline_caches(): the curve in world space (entity transform
// applied), evenly spaced samples (draped onto the ground when
// follow_ground) and bounds. Games and tools read the cache; they edit the
// Spline. The codec name is `spline`; the cache is never serialised.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <entt/entity/fwd.hpp>

#include "engine/spline/curve.hpp"
#include "engine/spline/ground.hpp" // GroundHeightFn (the type navigation.hpp declares)
#include "engine/spline/spline.hpp"

namespace engine::scene {

using Spline = spline::Spline;
using SplinePoint = spline::SplinePoint;
using SplineKind = spline::SplineKind;

struct SplineCache {
    spline::Curve                    curve;   // world space
    std::vector<spline::CurveSample> samples; // every `spacing` metres, both ends included
    spline::Bounds                   bounds{}; // world, swept by width / height (and draped samples)
    float                            spacing = 0.0F;
    std::uint64_t                    source_hash = 0; // spline + world matrix + spacing
    std::uint64_t                    revision = 0;    // bumped on every rebuild
    std::size_t                      missing_ground = 0; // samples over unloaded ground (retried)
    std::uint32_t                    retry_in = 0;       // updates until the next ground retry
    bool                             valid = false;
    std::string                      error; // why the spline does not compile (when !valid)
};

struct SplineCacheOptions {
    float sample_spacing = 0.5F; // metres between cached samples
};

// Rebuilds the SplineCache of every Spline entity whose spline, world
// transform or spacing changed, or whose samples still wait for ground (a
// streamed tile not loaded yet; retried every 30 updates). Removes caches of entities that lost their
// Spline. Returns how many caches were rebuilt.
std::size_t update_spline_caches(entt::registry& registry, const spline::GroundHeightFn& ground,
                                 const SplineCacheOptions& options = {});

// The spline of `e` in world space (its Transform::world applied; identity
// without a Transform).
[[nodiscard]] Spline world_spline(const entt::registry& registry, entt::entity e);

} // namespace engine::scene
