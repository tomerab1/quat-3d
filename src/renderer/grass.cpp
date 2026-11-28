#include "engine/renderer/grass.hpp"

#include <algorithm>
#include <cmath>
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

float grass_ground_keep(const GrassSettings& settings, float normal_y, float mask) {
    return grass_slope_keep(settings, normal_y) * std::clamp(mask, 0.0F, 1.0F);
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

GrassCandidate grass_candidate(const GrassSettings& settings, float cell_size, glm::ivec2 cell) {
    const glm::uvec3 h = grass_pcg3d(
        glm::uvec3(static_cast<std::uint32_t>(cell.x), static_cast<std::uint32_t>(cell.y),
                   settings.seed));
    constexpr float k_unit = 1.0F / 16777216.0F; // 24-bit mantissa-exact
    const float u = static_cast<float>(h.x >> 8U) * k_unit;
    const float v = static_cast<float>(h.y >> 8U) * k_unit;
    GrassCandidate c;
    c.root_xz = glm::vec2((static_cast<float>(cell.x) + u) * cell_size,
                          (static_cast<float>(cell.y) + v) * cell_size);
    c.threshold = static_cast<float>(h.z >> 8U) * k_unit;
    c.hash = (h.x ^ h.z) & 0xFFFFU;
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
