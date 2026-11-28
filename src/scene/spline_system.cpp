// SplineCache upkeep (ROADMAP G18.1): world-space curve, evenly spaced
// samples draped on the ground, bounds. See engine/scene/spline.hpp.

#include <algorithm>
#include <cstring>

#include <entt/entity/registry.hpp>

#include "engine/core/profile.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/spline.hpp"

namespace engine::scene {

namespace {

constexpr std::uint32_t k_ground_retry_interval = 30;

[[nodiscard]] std::uint64_t hash_floats(std::uint64_t h, const float* data, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &data[i], sizeof bits);
        h = (h ^ bits) * 1099511628211ULL;
    }
    return h;
}

[[nodiscard]] glm::mat4 world_of(const entt::registry& r, entt::entity e) {
    const auto* t = r.try_get<Transform>(e);
    return t != nullptr ? t->world : glm::mat4(1.0F);
}

void rebuild(SplineCache& cache, const Spline& world, const spline::GroundHeightFn& ground, float spacing) {
    cache.samples.clear();
    cache.missing_ground = 0;
    auto curve = spline::Curve::build(world);
    if (!curve) {
        cache.curve = {};
        cache.valid = false;
        cache.error = curve.error().message;
        cache.bounds = {};
        return;
    }
    cache.curve = std::move(*curve);
    cache.valid = true;
    cache.error.clear();
    cache.samples = spline::sample_by_spacing(cache.curve, {.spacing = spacing, .fit_to_ends = true});
    spline::Bounds b = cache.curve.swept_bounds();
    if (world.follow_ground && ground && !cache.samples.empty()) {
        cache.missing_ground = spline::drape(cache.samples, ground, world.ground_offset);
        // The draped samples replace the curve's own heights.
        float top = 0.0F;
        float bottom = 0.0F;
        for (const spline::Segment& s : cache.curve.segments()) {
            top = std::max({top, s.height[0], s.height[1]});
            bottom = std::min({bottom, s.height[0], s.height[1]});
        }
        b.min.y = cache.samples[0].position.y;
        b.max.y = b.min.y;
        for (const spline::CurveSample& s : cache.samples) {
            b.min.y = std::min(b.min.y, s.position.y);
            b.max.y = std::max(b.max.y, s.position.y);
        }
        b.min.y += bottom;
        b.max.y += top;
    }
    cache.bounds = b;
}

} // namespace

Spline world_spline(const entt::registry& r, entt::entity e) {
    const auto* s = r.try_get<Spline>(e);
    if (s == nullptr) return {};
    return spline::transformed(*s, world_of(r, e));
}

std::size_t update_spline_caches(entt::registry& r, const spline::GroundHeightFn& ground,
                                 const SplineCacheOptions& options) {
    QUAT_ZONE_C("spline_cache_system", core::ProfileCategory::system);
    const float spacing = std::clamp(options.sample_spacing, 0.01F, 100.0F);
    std::size_t rebuilt = 0;
    for (const entt::entity e : r.view<const Spline>()) {
        const Spline& s = r.get<const Spline>(e);
        const glm::mat4 m = world_of(r, e);
        std::uint64_t h = spline::content_hash(s);
        h = hash_floats(h, &m[0][0], 16);
        h = hash_floats(h, &spacing, 1);
        auto* cache = r.try_get<SplineCache>(e);
        if (cache != nullptr && cache->source_hash == h) {
            if (cache->missing_ground == 0 || !ground) continue;
            if (cache->retry_in > 0) {
                --cache->retry_in;
                continue;
            }
        }
        if (cache == nullptr) cache = &r.emplace<SplineCache>(e);
        rebuild(*cache, spline::transformed(s, m), ground, spacing);
        cache->spacing = spacing;
        cache->source_hash = h;
        cache->retry_in = cache->missing_ground > 0 ? k_ground_retry_interval : 0;
        ++cache->revision;
        ++rebuilt;
    }
    // Caches whose spline was removed.
    std::vector<entt::entity> stale;
    for (const entt::entity e : r.view<const SplineCache>(entt::exclude<Spline>)) stale.push_back(e);
    for (const entt::entity e : stale) r.remove<SplineCache>(e);
    return rebuilt;
}

} // namespace engine::scene
