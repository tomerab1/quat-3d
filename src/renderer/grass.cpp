#include "engine/renderer/grass.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::renderer {

namespace {

[[nodiscard]] float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0F : 1.0F;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] glm::vec2 ordered(glm::vec2 r, float lo, float hi) {
    r = glm::clamp(r, glm::vec2(lo), glm::vec2(hi));
    if (r.y < r.x) std::swap(r.x, r.y);
    return r;
}

// [0, 1) from the top 24 bits (mantissa-exact; unit_floats in grass_common.slang).
[[nodiscard]] float unit_float(std::uint32_t h) {
    return static_cast<float>(h >> 8U) * (1.0F / 16777216.0F);
}

// Upper bound on the cells any point of which lies within `radius` (in cell
// units): every such cell fits in the disc of radius + 2 (cell diagonal < 2).
[[nodiscard]] std::uint64_t disc_cells(float radius_cells) {
    const double r = static_cast<double>(radius_cells) + 2.0;
    return static_cast<std::uint64_t>(std::ceil(std::numbers::pi * r * r));
}

} // namespace

GrassSettings sanitize(const GrassSettings& in) {
    GrassSettings s = in;
    s.density = std::clamp(std::isfinite(s.density) ? s.density : 0.0F, 0.0F, 1000.0F);
    s.radius = std::clamp(std::isfinite(s.radius) ? s.radius : 0.0F, 0.0F, 500.0F);
    s.height_range = ordered(s.height_range, 0.01F, 5.0F);
    s.width_range = ordered(s.width_range, 0.001F, 0.5F);
    s.base_color = glm::clamp(s.base_color, glm::vec3(0.0F), glm::vec3(1.0F));
    s.tip_color = glm::clamp(s.tip_color, glm::vec3(0.0F), glm::vec3(1.0F));
    const float wind_len = glm::length(s.wind_direction);
    s.wind_direction = std::isfinite(wind_len) && wind_len > 1e-6F ? s.wind_direction / wind_len
                                                                   : glm::vec2(1.0F, 0.0F);
    s.wind_strength = std::clamp(s.wind_strength, 0.0F, 4.0F);
    s.wind_frequency = std::clamp(s.wind_frequency, 0.0F, 10.0F);
    s.slope_deg = ordered(s.slope_deg, 0.0F, 90.0F);
    s.height_m = ordered(s.height_m, -100000.0F, 100000.0F);
    s.lod_start = std::clamp(s.lod_start, 0.0F, 1.0F);
    s.far_density = std::clamp(s.far_density, 0.0F, 1.0F);
    const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0F, 1.0F) : 0.0F; };
    s.clump_size = std::isfinite(s.clump_size) ? std::clamp(s.clump_size, 0.0F, 8.0F) : 0.0F;
    s.clump_strength = std::isfinite(s.clump_strength) ? std::clamp(s.clump_strength, 0.0F, 0.9F)
                                                       : 0.0F;
    s.color_variation = unit(s.color_variation);
    s.dry_color = glm::clamp(s.dry_color, glm::vec3(0.0F), glm::vec3(1.0F));
    s.dry_fraction = unit(s.dry_fraction);
    s.root_tint = unit(s.root_tint);
    s.ground_cover_color = glm::clamp(s.ground_cover_color, glm::vec3(0.0F), glm::vec3(1.0F));
    s.ground_cover = unit(s.ground_cover);
    s.far_ground_cover = unit(s.far_ground_cover);
    s.min_blade_px = std::isfinite(s.min_blade_px) ? std::clamp(s.min_blade_px, 0.0F, 8.0F) : 0.0F;
    return s;
}

float grass_cell_size(const GrassSettings& settings) {
    if (settings.density <= 0.0F || settings.radius <= 0.0F) return 0.0F;
    float cell = 1.0F / std::sqrt(settings.density);
    // Side of the square grid around the disc, in cells; coarsen until the
    // dispatch fits the per-frame candidate budget.
    const auto cells_for = [&](float c) {
        const auto side = static_cast<std::uint64_t>(2.0F * std::ceil(settings.radius / c) + 1.0F);
        return side * side;
    };
    while (cells_for(cell) > grass_max_cells) cell *= 1.1F;
    return cell;
}

GrassGrid grass_grid(const GrassSettings& settings, const glm::vec3& camera) {
    GrassGrid g;
    g.cell_size = grass_cell_size(settings);
    if (g.cell_size <= 0.0F) return g;
    const auto half = static_cast<std::int32_t>(std::ceil(settings.radius / g.cell_size));
    const auto cx = static_cast<std::int32_t>(std::floor(camera.x / g.cell_size));
    const auto cz = static_cast<std::int32_t>(std::floor(camera.z / g.cell_size));
    g.origin = glm::ivec2(cx - half, cz - half);
    const auto side = static_cast<std::uint32_t>(2 * half + 1);
    g.groups = glm::uvec2((side + grass_tile_cells - 1) / grass_tile_cells);
    g.cells = g.groups * grass_tile_cells;
    return g;
}

float grass_thinning(const GrassSettings& settings, float distance) {
    const float t = smoothstep(settings.lod_start * settings.radius, settings.radius, distance);
    return 1.0F + (settings.far_density - 1.0F) * t;
}

float grass_keep_fraction(const GrassSettings& settings, float distance) {
    if (distance >= settings.radius) return 0.0F;
    const float edge = 1.0F - smoothstep(settings.radius * (1.0F - grass_edge_band),
                                         settings.radius, distance);
    return grass_thinning(settings, distance) * edge;
}

float grass_footprint_keep(const GrassSettings& settings, float distance,
                           float pixels_per_metre_at_1m) {
    if (settings.min_blade_px <= 0.0F || pixels_per_metre_at_1m <= 0.0F || distance <= 0.0F) {
        return 1.0F;
    }
    const float width = 0.5F * (settings.width_range.x + settings.width_range.y);
    const float px = width * pixels_per_metre_at_1m / distance;
    return std::clamp(px / settings.min_blade_px, grass_min_footprint_keep, 1.0F);
}

float grass_width_scale(float thinning) {
    return std::min(1.0F / std::max(thinning, 1e-4F), grass_max_width_scale);
}

float grass_slope_fade_start(const GrassSettings& settings) {
    if (settings.slope_deg.y >= 90.0F) return -2.0F;
    const float start_deg =
        std::max(settings.slope_deg.x, settings.slope_deg.y - grass_slope_fade_deg);
    return std::cos(start_deg * (std::numbers::pi_v<float> / 180.0F));
}

float grass_slope_keep(const GrassSettings& settings, float normal_y) {
    const float start = grass_slope_fade_start(settings);
    if (start <= -1.5F) return 1.0F;
    const float limit = std::cos(settings.slope_deg.y * (std::numbers::pi_v<float> / 180.0F));
    if (start <= limit) return normal_y >= limit ? 1.0F : 0.0F; // no band (min == max)
    return smoothstep(limit, start, normal_y);
}

GrassCapacity grass_capacity(const GrassSettings& settings) {
    const float cell = grass_cell_size(settings);
    if (cell <= 0.0F) return {};
    const std::uint64_t all = disc_cells(settings.radius / cell);
    const std::uint64_t near =
        std::min(all, disc_cells(settings.lod_start * settings.radius / cell));
    GrassCapacity c;
    c.near = static_cast<std::uint32_t>(std::min<std::uint64_t>(near, grass_max_blades / 2));
    c.far = static_cast<std::uint32_t>(std::min<std::uint64_t>(all, grass_max_blades - c.near));
    return c;
}

glm::uvec3 grass_pcg3d(glm::uvec3 v) {
    v = v * 1664525U + 1013904223U;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16U;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// The nearest feature point of a jittered grid over the 3x3 neighbouring
// cells (Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996).
GrassClump grass_clump(const GrassSettings& settings, glm::vec2 xz) {
    GrassClump out;
    if (settings.clump_size <= 0.0F) return out;
    const float size = settings.clump_size;
    const glm::ivec2 home(static_cast<int>(std::floor(xz.x / size)),
                          static_cast<int>(std::floor(xz.y / size)));
    float best = std::numeric_limits<float>::max();
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const glm::ivec2 c = home + glm::ivec2(dx, dz);
            const glm::uvec3 h = grass_pcg3d(glm::uvec3(static_cast<std::uint32_t>(c.x),
                                                        static_cast<std::uint32_t>(c.y),
                                                        settings.seed ^ grass_clump_salt));
            const glm::vec2 centre((static_cast<float>(c.x) + unit_float(h.x)) * size,
                                   (static_cast<float>(c.y) + unit_float(h.y)) * size);
            const glm::vec2 d = centre - xz;
            const float d2 = d.x * d.x + d.y * d.y;
            if (d2 < best) { // first wins on ties (same order as the shader)
                best = d2;
                out.centre = centre;
                out.hash = h.z;
            }
        }
    }
    return out;
}

float grass_max_blade_height(const GrassSettings& settings) {
    return settings.height_range.y *
           (settings.clump_size > 0.0F ? grass_clump_max_height_scale : 1.0F);
}

float grass_max_clump_pull(const GrassSettings& settings) {
    // The chosen centre is never farther than the home cell's own centre,
    // which lies within one cell diagonal of the point.
    return settings.clump_size * settings.clump_strength * std::numbers::sqrt2_v<float>;
}

GrassCandidate grass_candidate(const GrassSettings& settings, float cell_size, glm::ivec2 cell) {
    const glm::uvec3 h = grass_pcg3d(
        glm::uvec3(static_cast<std::uint32_t>(cell.x), static_cast<std::uint32_t>(cell.y),
                   settings.seed));
    GrassCandidate c;
    c.root_xz = glm::vec2((static_cast<float>(cell.x) + unit_float(h.x)) * cell_size,
                          (static_cast<float>(cell.y) + unit_float(h.y)) * cell_size);
    c.threshold = unit_float(h.z);
    c.hash = (h.x ^ h.z) & 0xFFFFU;
    if (settings.clump_size > 0.0F) {
        const GrassClump clump = grass_clump(settings, c.root_xz);
        c.root_xz += (clump.centre - c.root_xz) * settings.clump_strength;
        c.clump_hash = clump.hash;
    }
    return c;
}

float grass_blade_fade(float keep, float threshold) {
    const float t = threshold * (1.0F - grass_fade_band);
    if (t >= keep) return 0.0F;
    return std::clamp((keep - t) / grass_fade_band, 0.0F, 1.0F);
}

std::array<glm::vec4, 6> grass_frustum_planes(const glm::mat4& view_proj) {
    const glm::mat4 m = glm::transpose(view_proj);
    std::array<glm::vec4, 6> planes{m[3] + m[0], m[3] - m[0], m[3] + m[1],
                                    m[3] - m[1], m[2],        m[3] - m[2]};
    for (glm::vec4& p : planes) {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0F) p /= len;
    }
    return planes;
}

} // namespace engine::renderer
