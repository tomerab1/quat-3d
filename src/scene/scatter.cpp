// Procedural scattering — see include/engine/scene/scatter.hpp.

#include "engine/scene/scatter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/terrain/heightmap.hpp"
#include "fnv_hash.hpp"

namespace engine::scene {

namespace {

// PCG32 (O'Neill, pcg32_random_r): a small, fast generator with a fixed,
// platform-independent sequence per (seed, stream) — unlike the standard
// distributions, whose output is implementation-defined.
class Pcg32 {
public:
    Pcg32(std::uint64_t seed, std::uint64_t stream) : inc_((stream << 1U) | 1U) {
        (void)next();
        state_ += seed;
        (void)next();
    }

    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
        const auto rot = static_cast<std::uint32_t>(old >> 59U);
        return (xorshifted >> rot) | (xorshifted << ((32U - rot) & 31U));
    }

    // Uniform in [0, 1) with 24 bits of precision (exact in a float).
    float unit() { return static_cast<float>(next() >> 8U) * (1.0F / 16777216.0F); }

    // Uniform integer in [0, bound), unbiased (Lemire's multiply-shift with
    // rejection). bound > 0.
    std::uint32_t below(std::uint32_t bound) {
        std::uint64_t m = static_cast<std::uint64_t>(next()) * bound;
        auto low = static_cast<std::uint32_t>(m);
        if (low < bound) {
            const std::uint32_t threshold = (0U - bound) % bound;
            while (low < threshold) {
                m = static_cast<std::uint64_t>(next()) * bound;
                low = static_cast<std::uint32_t>(m);
            }
        }
        return static_cast<std::uint32_t>(m >> 32U);
    }

private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_;
};

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

[[nodiscard]] float unit_from_bits(std::uint64_t bits) {
    return static_cast<float>(bits >> 40U) * (1.0F / 16777216.0F);
}

// Independent PCG streams for the stages that consume randomness.
constexpr std::uint64_t k_stream_points = 0x5CA77E50001ULL;
constexpr std::uint64_t k_stream_thin = 0x5CA77E50002ULL;

// Bridson's samples per active point before it retires (the paper's k).
constexpr int k_bridson_attempts = 30;
// Bridson with k = 30 settles near 0.65 / r^2 points per m^2. The Poisson
// radius used for a target density is sqrt(k_packing / density) — slightly
// denser than the target — and the surplus is thinned away at random, which
// keeps the blue-noise spacing while hitting the requested count.
constexpr float k_packing = 0.5F;

[[nodiscard]] glm::vec2 half_extent(const Scatter& s) {
    if (s.area == ScatterArea::circle) return glm::vec2(glm::max(s.radius, 0.0F));
    return glm::max(s.size, glm::vec2(0.0F)) * 0.5F;
}

[[nodiscard]] double area_m2(const Scatter& s) {
    if (s.area == ScatterArea::circle) {
        const double r = glm::max(s.radius, 0.0F);
        return std::numbers::pi * r * r;
    }
    const glm::vec2 sz = glm::max(s.size, glm::vec2(0.0F));
    return static_cast<double>(sz.x) * static_cast<double>(sz.y);
}

[[nodiscard]] bool inside_area(const Scatter& s, const glm::vec2& p) {
    if (s.area != ScatterArea::circle) return true; // the domain is the rect
    return glm::dot(p, p) <= s.radius * s.radius;
}

// Poisson-disc sampling of the rectangle [-half, half] with radius `r`
// (Bridson 2007: grid-accelerated dart throwing in the annulus [r, 2r]
// around active samples). Stops early only at `cap` points.
//
// Active samples are processed in acceptance order (a FIFO front growing
// outward from the first sample), each spending all k attempts before it
// retires — Bridson allows any active-sample order, and this one keeps
// consecutive grid lookups close together in memory, which matters at tens
// of thousands of points. The k attempt directions are evenly spaced and
// rotated by a random start per sample (stratified angles: one sin/cos per
// sample instead of per attempt); radii are uniform by area.
[[nodiscard]] std::vector<glm::vec2> bridson(Pcg32& rng, glm::vec2 half, float r,
                                             std::size_t cap) {
    std::vector<glm::vec2> points;
    if (cap == 0 || r <= 0.0F || half.x <= 0.0F || half.y <= 0.0F) return points;
    const float cell = r / std::numbers::sqrt2_v<float>;
    const int gw = std::max(1, static_cast<int>(std::ceil(2.0F * half.x / cell)));
    const int gh = std::max(1, static_cast<int>(std::ceil(2.0F * half.y / cell)));
    std::vector<std::int32_t> grid(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh),
                                   -1);
    const auto cell_of = [&](const glm::vec2& p) {
        const int cx = std::clamp(static_cast<int>((p.x + half.x) / cell), 0, gw - 1);
        const int cy = std::clamp(static_cast<int>((p.y + half.y) / cell), 0, gh - 1);
        return glm::ivec2(cx, cy);
    };
    const auto slot = [&](const glm::ivec2& c) -> std::int32_t& {
        return grid[static_cast<std::size_t>(c.y) * static_cast<std::size_t>(gw) +
                    static_cast<std::size_t>(c.x)];
    };
    const float r2 = r * r;
    // Neighbour cells that can hold a sample closer than r: the 5x5 block
    // minus its corners (those lie at least one cell diagonal = r away),
    // nearest first — most attempts are rejected, and the nearest cells
    // reject them soonest.
    static constexpr std::array<glm::ivec2, 21> k_near = {{
        {0, 0},   {1, 0},   {-1, 0},  {0, 1},  {0, -1}, {1, 1},  {-1, 1},
        {1, -1},  {-1, -1}, {2, 0},   {-2, 0}, {0, 2},  {0, -2}, {2, 1},
        {2, -1},  {-2, 1},  {-2, -1}, {1, 2},  {-1, 2}, {1, -2}, {-1, -2},
    }};
    const auto fits = [&](const glm::vec2& q) {
        if (q.x < -half.x || q.x >= half.x || q.y < -half.y || q.y >= half.y) return false;
        const glm::ivec2 c = cell_of(q);
        for (const glm::ivec2& o : k_near) {
            const glm::ivec2 n = c + o;
            if (n.x < 0 || n.y < 0 || n.x >= gw || n.y >= gh) continue;
            const std::int32_t i = slot(n);
            if (i < 0) continue;
            const glm::vec2 d = points[static_cast<std::size_t>(i)] - q;
            if (glm::dot(d, d) < r2) return false;
        }
        return true;
    };
    const auto accept = [&](const glm::vec2& q) {
        slot(cell_of(q)) = static_cast<std::int32_t>(points.size());
        points.push_back(q);
    };

    constexpr float two_pi = 2.0F * std::numbers::pi_v<float>;
    std::array<glm::vec2, k_bridson_attempts> dirs{};
    for (int k = 0; k < k_bridson_attempts; ++k) {
        const float a = two_pi * static_cast<float>(k) / static_cast<float>(k_bridson_attempts);
        dirs[static_cast<std::size_t>(k)] = {std::cos(a), std::sin(a)};
    }

    accept({(rng.unit() * 2.0F - 1.0F) * half.x, (rng.unit() * 2.0F - 1.0F) * half.y});
    for (std::size_t head = 0; head < points.size() && points.size() < cap; ++head) {
        const glm::vec2 p = points[head];
        const float start = rng.unit() * two_pi;
        const float cs = std::cos(start);
        const float sn = std::sin(start);
        for (const glm::vec2& d : dirs) {
            const float dist = std::sqrt(r2 + rng.unit() * 3.0F * r2);
            const glm::vec2 q = p + dist * glm::vec2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
            if (fits(q)) {
                accept(q);
                if (points.size() >= cap) break;
            }
        }
    }
    return points;
}

// One sample per cell of a grid with cells >= max(1/sqrt(density), spacing),
// jittered inside the cell minus a spacing/2 margin, so neighbours in any
// direction stay at least `spacing` apart.
[[nodiscard]] std::vector<glm::vec2> jittered_grid(Pcg32& rng, glm::vec2 half, float density,
                                                   float spacing) {
    std::vector<glm::vec2> points;
    if (density <= 0.0F || half.x <= 0.0F || half.y <= 0.0F) return points;
    const float cell = std::max(1.0F / std::sqrt(density), spacing);
    const auto cells_along = [&](float extent) {
        int n = std::max(1, static_cast<int>(std::lround(extent / cell)));
        if (spacing > 0.0F && extent / static_cast<float>(n) < spacing) {
            n = std::max(1, static_cast<int>(extent / spacing)); // rounding shrank cells
        }
        return n;
    };
    const int nx = cells_along(2.0F * half.x);
    const int nz = cells_along(2.0F * half.y);
    const glm::vec2 c(2.0F * half.x / static_cast<float>(nx), 2.0F * half.y / static_cast<float>(nz));
    const glm::vec2 margin = glm::min(glm::vec2(spacing * 0.5F), c * 0.5F);
    points.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz));
    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            const glm::vec2 lo(-half.x + static_cast<float>(x) * c.x,
                               -half.y + static_cast<float>(z) * c.y);
            const glm::vec2 u(rng.unit(), rng.unit());
            points.push_back(lo + margin + u * (c - 2.0F * margin));
        }
    }
    return points;
}

[[nodiscard]] float sanitized_density(const Scatter& s) {
    return std::isfinite(s.density) ? glm::max(s.density, 0.0F) : 0.0F;
}

[[nodiscard]] float sanitized_spacing(const Scatter& s) {
    return std::isfinite(s.min_spacing) ? glm::max(s.min_spacing, 0.0F) : 0.0F;
}

void hash_scatter(Fnv64& h, const Scatter& s) {
    h.u32(s.seed);
    h.u32(static_cast<std::uint32_t>(s.area));
    h.f32(s.size.x);
    h.f32(s.size.y);
    h.f32(s.radius);
    h.f32(s.density);
    h.f32(s.min_spacing);
    h.u32(static_cast<std::uint32_t>(s.pattern));
    h.f32(s.scale_range.x);
    h.f32(s.scale_range.y);
    h.u32(s.random_yaw ? 1U : 0U);
    h.f32(s.align_to_normal);
    h.f32(s.base_offset);
    h.f32(s.mirror);
    h.f32(s.slope_deg.x);
    h.f32(s.slope_deg.y);
    h.f32(s.height_m.x);
    h.f32(s.height_m.y);
    h.u32(s.max_instances);
}

} // namespace

ScatterRect scatter_rect(const Scatter& scatter, const glm::vec3& centre) {
    const glm::vec2 half = half_extent(scatter);
    const glm::vec2 c(centre.x, centre.z);
    return {c - half, c + half};
}

std::vector<glm::vec2> scatter_candidates(const Scatter& s) {
    const double area = area_m2(s);
    const float density = sanitized_density(s);
    const float spacing = sanitized_spacing(s);
    const std::uint32_t limit = std::min(s.max_instances, k_scatter_instance_limit);
    if (area <= 0.0 || density <= 0.0F || limit == 0) return {};

    const double wanted = std::round(static_cast<double>(density) * area);
    const auto target = static_cast<std::size_t>(std::min(wanted, static_cast<double>(limit)));
    if (target == 0) return {};
    // The density the target corresponds to (lower than requested when the
    // instance cap binds), which sizes the Poisson radius / grid cells.
    const auto effective = static_cast<float>(static_cast<double>(target) / area);

    const glm::vec2 half = half_extent(s);
    Pcg32 rng(s.seed, k_stream_points);
    std::vector<glm::vec2> points;
    if (s.pattern == ScatterPattern::jittered_grid) {
        points = jittered_grid(rng, half, effective, spacing);
    } else {
        const float radius = std::max(spacing, std::sqrt(k_packing / effective));
        // Bounded by the radius (at most ~0.7/r^2 per m^2 of the bounding
        // square); the cap only guards against a pathological sequence.
        points = bridson(rng, half, radius, 3U * static_cast<std::size_t>(k_scatter_instance_limit));
    }
    if (s.area == ScatterArea::circle) {
        std::erase_if(points, [&](const glm::vec2& p) { return !inside_area(s, p); });
    }

    // Thin to the target: a seeded partial Fisher-Yates picks which points
    // stay; survivors keep their generation order. Removing points never
    // breaks the minimum spacing.
    if (points.size() > target) {
        Pcg32 thin(s.seed, k_stream_thin);
        std::vector<std::uint32_t> order(points.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        for (std::size_t i = 0; i < target; ++i) {
            const std::size_t j =
                i + thin.below(static_cast<std::uint32_t>(order.size() - i));
            std::swap(order[i], order[j]);
        }
        order.resize(target);
        std::sort(order.begin(), order.end());
        std::vector<glm::vec2> kept;
        kept.reserve(target);
        for (const std::uint32_t i : order) kept.push_back(points[i]);
        points = std::move(kept);
    }
    return points;
}

std::vector<InstanceTransform> generate_scatter(const Scatter& s, const glm::mat4& entity_world,
                                                const ScatterGround& ground) {
    std::vector<InstanceTransform> out;
    // The entity frame the placements are expressed in. A singular or
    // non-finite transform has no inverse: nothing can be placed.
    const float det = glm::determinant(glm::mat3(entity_world));
    if (!std::isfinite(det) || std::abs(det) < 1e-12F) return out;
    const glm::mat4 to_local = glm::inverse(entity_world);
    const glm::vec3 axis_scale(glm::length(glm::vec3(entity_world[0])),
                               glm::length(glm::vec3(entity_world[1])),
                               glm::length(glm::vec3(entity_world[2])));
    const glm::mat3 entity_rot(glm::vec3(entity_world[0]) / axis_scale.x,
                               glm::vec3(entity_world[1]) / axis_scale.y,
                               glm::vec3(entity_world[2]) / axis_scale.z);
    const glm::quat to_local_rot = glm::conjugate(glm::normalize(glm::quat_cast(entity_rot)));
    const glm::vec3 centre(entity_world[3]);

    const std::vector<glm::vec2> candidates = scatter_candidates(s);
    out.reserve(candidates.size());
    const glm::vec3 up_axis(0.0F, 1.0F, 0.0F);
    const float align = glm::clamp(s.align_to_normal, 0.0F, 1.0F);
    const float scale_lo = glm::min(s.scale_range.x, s.scale_range.y);
    const float scale_hi = glm::max(s.scale_range.x, s.scale_range.y);
    constexpr float slope_eps = 1e-3F; // degrees: keep exactly-at-the-limit flats

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const float x = centre.x + candidates[i].x;
        const float z = centre.z + candidates[i].y;
        GroundSample g{centre.y, up_axis};
        if (ground.sample) {
            const std::optional<GroundSample> hit = ground.sample(x, z);
            if (!hit) continue; // no ground known here (yet)
            g = *hit;
        }
        const float n_len = glm::length(g.normal);
        const glm::vec3 n = n_len > 1e-6F ? g.normal / n_len : up_axis;
        const float slope = glm::degrees(std::acos(glm::clamp(n.y, -1.0F, 1.0F)));
        if (slope < s.slope_deg.x - slope_eps || slope > s.slope_deg.y + slope_eps) continue;
        if (g.height < s.height_m.x || g.height > s.height_m.y) continue;

        // Per-candidate randomness from (seed, index): independent of which
        // other candidates survived the ground tests.
        const std::uint64_t h0 =
            splitmix64((static_cast<std::uint64_t>(s.seed) << 32U) ^ static_cast<std::uint64_t>(i));
        const std::uint64_t h1 = splitmix64(h0);
        const float scale = glm::mix(scale_lo, scale_hi, unit_from_bits(h0));
        const float yaw =
            s.random_yaw ? unit_from_bits(h1) * 2.0F * std::numbers::pi_v<float> : 0.0F;
        const bool mirrored = unit_from_bits(splitmix64(h1)) < s.mirror;

        const glm::vec3 blended = glm::mix(up_axis, n, align);
        const glm::vec3 up = glm::length(blended) > 1e-6F ? glm::normalize(blended) : up_axis;
        const glm::quat tilt(up_axis, up); // shortest arc +Y -> up
        const glm::quat world_rot = tilt * glm::angleAxis(yaw, up_axis);
        const glm::vec3 world_pos = glm::vec3(x, g.height, z) + up * (s.base_offset * scale);

        InstanceTransform t;
        t.position = glm::vec3(to_local * glm::vec4(world_pos, 1.0F));
        t.rotation = glm::normalize(to_local_rot * world_rot);
        t.scale = glm::vec3(scale) / axis_scale;
        if (mirrored) t.scale.x = -t.scale.x;
        out.push_back(t);
    }
    return out;
}

std::uint64_t scatter_key(const Scatter& scatter, const glm::mat4& entity_world,
                          std::uint64_t ground_revision) {
    Fnv64 h;
    hash_scatter(h, scatter);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) h.f32(entity_world[c][r]);
    }
    h.u64(ground_revision);
    return h.nonzero();
}

ScatterGround make_tile_ground(std::vector<GroundTile> tiles) {
    std::erase_if(tiles, [](const GroundTile& t) { return t.map == nullptr || !t.map->valid(); });
    auto shared = std::make_shared<const std::vector<GroundTile>>(std::move(tiles));

    ScatterGround ground;
    ground.sample = [shared](float x, float z) -> std::optional<GroundSample> {
        for (const GroundTile& t : *shared) {
            const float lx = x - t.origin.x;
            const float lz = z - t.origin.z;
            const float size = t.map->tile_size_m;
            if (lx < 0.0F || lz < 0.0F || lx > size || lz > size) continue;
            return GroundSample{t.origin.y + t.map->sample(lx, lz), t.map->normal(lx, lz)};
        }
        return std::nullopt;
    };
    ground.revision = [shared](glm::vec2 min_xz, glm::vec2 max_xz) -> std::uint64_t {
        Fnv64 h;
        for (const GroundTile& t : *shared) {
            const float size = t.map->tile_size_m;
            if (t.origin.x > max_xz.x || t.origin.z > max_xz.y || t.origin.x + size < min_xz.x ||
                t.origin.z + size < min_xz.y) {
                continue;
            }
            h.f32(t.origin.x);
            h.f32(t.origin.y);
            h.f32(t.origin.z);
            h.u32(t.map->resolution);
            h.f32(size);
            h.f32(t.map->min_height);
            h.f32(t.map->max_height);
            // A sparse fingerprint of the heights (regenerated tiles with
            // identical placement and range still differ here).
            const std::size_t n = t.map->heights.size();
            constexpr std::size_t k_probes = 64;
            for (std::size_t k = 0; k < k_probes; ++k) h.f32(t.map->heights[(k * n) / k_probes]);
        }
        return h.value();
    };
    return ground;
}

std::size_t scatter_system(entt::registry& registry, const ScatterGround& ground) {
    std::size_t regenerated = 0;
    for (auto [e, scatter, transform] : registry.view<Scatter, const Transform>().each()) {
        const glm::vec3 centre(transform.world[3]);
        const ScatterRect rect = scatter_rect(scatter, centre);
        const std::uint64_t revision = ground.revision ? ground.revision(rect.min, rect.max) : 0;
        const std::uint64_t key = scatter_key(scatter, transform.world, revision);
        if (key == scatter.generated) continue;
        auto& mesh = registry.get_or_emplace<InstancedMesh>(e);
        mesh.instances = generate_scatter(scatter, transform.world, ground);
        scatter.generated = key;
        ++scatter.generation;
        ++regenerated;
    }
    return regenerated;
}

} // namespace engine::scene
