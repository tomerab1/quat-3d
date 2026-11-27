// Wind field CPU reference — mirrored operation for operation by
// assets/shaders/lib/wind_common.slang (keep the two in sync; the pass's
// entry point is assets/shaders/wind_field.slang).

#include "engine/weather/wind_field.hpp"

#include <algorithm>
#include <cmath>

#include "engine/weather/weather_state.hpp"

namespace engine::weather {

namespace {

constexpr std::uint32_t k_wrap = k_wind_lattice_period - 1U;

// pcg3d (Jarzynski & Olano 2020), as in the grass shaders.
[[nodiscard]] glm::uvec3 pcg3d(glm::uvec3 v) {
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

// Lattice value in [0, 1) at wrapped cell (x, y, z) of noise channel `seed`.
[[nodiscard]] float lattice(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t seed) {
    const glm::uvec3 h = pcg3d(glm::uvec3(x & k_wrap, y & k_wrap, (z & k_wrap) + seed * 1024U));
    return static_cast<float>(h.x >> 8U) * (1.0F / 16777216.0F);
}

[[nodiscard]] std::uint32_t cell(float floored) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(floored));
}

[[nodiscard]] float fade(float t) { return t * t * (3.0F - 2.0F * t); }

[[nodiscard]] float mix(float a, float b, float t) { return a * (1.0F - t) + b * t; }

// Periodic 2D value noise in [0, 1).
[[nodiscard]] float value_noise2(glm::vec2 p, std::uint32_t seed) {
    const glm::vec2 i = glm::floor(p);
    const glm::vec2 f = p - i;
    const std::uint32_t x0 = cell(i.x);
    const std::uint32_t y0 = cell(i.y);
    const float a = lattice(x0, y0, 0U, seed);
    const float b = lattice(x0 + 1U, y0, 0U, seed);
    const float c = lattice(x0, y0 + 1U, 0U, seed);
    const float d = lattice(x0 + 1U, y0 + 1U, 0U, seed);
    const float u = fade(f.x);
    const float v = fade(f.y);
    return mix(mix(a, b, u), mix(c, d, u), v);
}

// Periodic 3D value noise in [0, 1).
[[nodiscard]] float value_noise3(glm::vec3 p, std::uint32_t seed) {
    const glm::vec3 i = glm::floor(p);
    const glm::vec3 f = p - i;
    const std::uint32_t x0 = cell(i.x);
    const std::uint32_t y0 = cell(i.y);
    const std::uint32_t z0 = cell(i.z);
    const float u = fade(f.x);
    const float v = fade(f.y);
    const float w = fade(f.z);
    const float lo = mix(mix(lattice(x0, y0, z0, seed), lattice(x0 + 1U, y0, z0, seed), u),
                         mix(lattice(x0, y0 + 1U, z0, seed), lattice(x0 + 1U, y0 + 1U, z0, seed), u),
                         v);
    const float hi =
        mix(mix(lattice(x0, y0, z0 + 1U, seed), lattice(x0 + 1U, y0, z0 + 1U, seed), u),
            mix(lattice(x0, y0 + 1U, z0 + 1U, seed), lattice(x0 + 1U, y0 + 1U, z0 + 1U, seed), u),
            v);
    return mix(lo, hi, w);
}

[[nodiscard]] float wrapped(double cells) {
    const double period = static_cast<double>(k_wind_lattice_period);
    double r = std::fmod(cells, period);
    if (r < 0.0) r += period;
    return static_cast<float>(r);
}

} // namespace

WindFieldParams wind_field_params(const WeatherState& state, const WindAdvection& clock) {
    const WeatherState s = sanitized(state);
    WindFieldParams p;
    const float heading = glm::radians(s.wind_dir_deg);
    p.direction = {std::cos(heading), std::sin(heading)};
    p.speed = s.wind_speed;
    p.gustiness = s.gustiness;
    p.turbulence = s.turbulence;
    p.gust_offset = wrapped(clock.distance_m / static_cast<double>(k_gust_along_m));
    p.swirl_offset = wrapped(clock.distance_m / static_cast<double>(k_turbulence_m));
    p.swirl_phase = wrapped(clock.time_s / static_cast<double>(k_turbulence_evolve_s));
    return p;
}

WindSample sample_wind(const WindFieldParams& p, glm::vec2 xz) {
    constexpr float inv_gust_along = 1.0F / k_gust_along_m;
    constexpr float inv_gust_across = 1.0F / k_gust_across_m;
    constexpr float inv_swirl = 1.0F / k_turbulence_m;
    const glm::vec2 dir = p.direction;
    const float along = xz.x * dir.x + xz.y * dir.y;
    const float across = xz.y * dir.x - xz.x * dir.y;

    // Gust fronts: bands across the wind sweeping downwind.
    const glm::vec2 g(along * inv_gust_along - p.gust_offset, across * inv_gust_across);
    const float n = 0.65F * value_noise2(g, 1U) +
                    0.35F * value_noise2(g * 2.0F + glm::vec2(17.0F, 29.0F), 2U);
    const float t = std::clamp((n - 0.5F) * 3.5714286F, 0.0F, 1.0F);
    const float front = t * t * (3.0F - 2.0F * t);
    const float gust = 1.0F + p.gustiness * (1.6F * front - 0.35F);

    // Turbulence: an evolving swirl drifting with the mean flow.
    const glm::vec3 q(along * inv_swirl - p.swirl_offset, across * inv_swirl, p.swirl_phase);
    const float sx = value_noise3(q, 3U) * 2.0F - 1.0F;
    const float sz = value_noise3(q, 4U) * 2.0F - 1.0F;
    const float sv = value_noise3(q, 5U) * 2.0F - 1.0F;
    const float swirl = p.speed * p.turbulence * (0.6F + 0.4F * gust);

    const float h_along = p.speed * gust + swirl * 0.45F * sx;
    const float h_across = swirl * 0.45F * sz;
    WindSample out;
    out.velocity = {dir.x * h_along - dir.y * h_across, p.speed * p.turbulence * 0.12F * sv,
                    dir.y * h_along + dir.x * h_across};
    out.gust = gust;
    return out;
}

glm::vec3 wind_at(const WeatherState& state, const glm::vec3& position, double time_s) {
    const WeatherState s = sanitized(state);
    WindAdvection clock;
    clock.distance_m = static_cast<double>(s.wind_speed) * time_s;
    clock.time_s = time_s;
    return sample_wind(wind_field_params(s, clock), {position.x, position.z}).velocity;
}

float wind_profile(float height_above_ground_m) {
    const float z0 = k_wind_roughness_length_m;
    const float h = std::clamp(std::isfinite(height_above_ground_m) ? height_above_ground_m : 0.0F,
                               1.5F * z0, k_surface_layer_top_m);
    return std::log(h / z0) / std::log(k_wind_reference_height_m / z0);
}

} // namespace engine::weather
