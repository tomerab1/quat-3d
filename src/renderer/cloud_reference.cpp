// The cloud layer's constants and the CPU mirror of lib/clouds.slang — see
// include/engine/renderer/cloud_reference.hpp. Every function follows its
// Slang twin operation for operation.

#include "engine/renderer/cloud_reference.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::renderer {

namespace {

// Tile sizes relative to the base feature size (CloudSettings::size_km):
// the base volume's Perlin octave spans tile / 4 (about 2.5 x size_km / 4
// per cloud cluster), its Worley octaves tile / 8 .. tile / 128 (billows).
constexpr float k_base_tile_per_size = 2.5F;
constexpr float k_detail_tile_per_base = 1.0F / 14.0F;
constexpr float k_weather_tile_per_size = 7.0F;
constexpr float k_curl_km = 0.25F;     // detail displacement by the curl field
constexpr float k_cirrus_tile_km = 45.0F;
constexpr float k_cirrus_drift = 2.5F; // jet-stream winds aloft vs the cumulus drift
constexpr float k_detail_drift = 0.35F; // detail churns faster than the base

[[nodiscard]] float saturate(float v) { return std::clamp(v, 0.0F, 1.0F); }
[[nodiscard]] float mix(float a, float b, float t) { return a + (b - a) * t; }
[[nodiscard]] float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0F - 2.0F * t);
}

} // namespace

CloudLayerGpu make_cloud_layer(const CloudSettings& c, bool world_anchored,
                               const glm::vec3& camera, const CloudLights& lights) {
    CloudLayerGpu l;
    const glm::vec2 camera_km = glm::vec2(camera.x, camera.z) * 0.001F;
    glm::vec2 anchor = c.offset_km;
    glm::vec2 drift{0.0F};
    if (!world_anchored) {
        const float heading = glm::radians(c.wind_dir_deg);
        drift = glm::vec2(std::cos(heading), std::sin(heading)) * (c.wind_speed * c.time_s * 0.001F);
        anchor = camera_km - drift;
    } else {
        drift = camera_km - c.offset_km;
    }
    const float coverage = c.enabled ? saturate(c.coverage) : 0.0F;
    // A closing sky turns convective cells into layered cloud.
    const float type = mix(saturate(c.type), 0.3F, smoothstep(0.75F, 1.0F, coverage));
    const float size = std::max(c.size_km, 0.5F);
    const float base_tile = size * k_base_tile_per_size;
    l.shell = {std::max(c.bottom_km, 0.05F), std::max(c.thickness_km, 0.1F), coverage, type};
    l.medium = {std::max(c.density, 0.0F), k_cloud_albedo, saturate(c.detail),
                saturate(c.precipitation)};
    l.scale = {1.0F / base_tile, 1.0F / (base_tile * k_detail_tile_per_base),
               1.0F / (size * k_weather_tile_per_size), k_curl_km};
    l.offset = {anchor.x, anchor.y, -drift.x * k_detail_drift, -drift.y * k_detail_drift};
    const float cirrus = c.enabled ? saturate(c.cirrus_coverage) : 0.0F;
    l.cirrus = {cirrus, std::max(c.cirrus_altitude_km, c.bottom_km + c.thickness_km + 0.5F),
                std::max(c.cirrus_opacity, 0.0F), 1.0F / k_cirrus_tile_km};
    const glm::vec2 cirrus_anchor = camera_km - drift * k_cirrus_drift;
    l.cirrus_offset = {cirrus_anchor.x, cirrus_anchor.y, glm::length(drift) * 0.5F, 0.0F};
    l.octaves = {k_cloud_octave_a, k_cloud_octave_b, k_cloud_octave_c,
                 static_cast<float>(k_cloud_octaves)};
    l.phase = {0.8F, -0.3F, 0.25F, 1.0F};
    const glm::vec3 sun = glm::length(lights.sun_to) > 1e-6F ? glm::normalize(lights.sun_to)
                                                               : glm::vec3(0.0F, 1.0F, 0.0F);
    const glm::vec3 moon = glm::length(lights.moon_to) > 1e-6F ? glm::normalize(lights.moon_to)
                                                                : glm::vec3(0.0F, -1.0F, 0.0F);
    l.sun = {sun, std::max(lights.sun_irradiance, 0.0F)};
    l.moon = {moon, std::max(lights.moon_irradiance, 0.0F)};
    return l;
}

namespace cloud_reference {

float hg_phase(float cos_theta, float g) {
    const float g2 = g * g;
    return (1.0F - g2) / (4.0F * std::numbers::pi_v<float> *
                          std::pow(std::max(1.0F + g2 - 2.0F * g * cos_theta, 1e-6F), 1.5F));
}

float cloud_phase(const CloudLayerGpu& l, float cos_theta, float c_pow) {
    return mix(hg_phase(cos_theta, l.phase.x * c_pow), hg_phase(cos_theta, l.phase.y * c_pow),
               l.phase.z);
}

float height_profile(float h, float type) {
    using cloud_noise::remap;
    const float stratus = saturate(remap(h, 0.0F, 0.07F, 0.0F, 1.0F)) *
                          saturate(remap(h, 0.15F, 0.3F, 1.0F, 0.0F));
    const float strato = saturate(remap(h, 0.0F, 0.2F, 0.0F, 1.0F)) *
                         saturate(remap(h, 0.35F, 0.65F, 1.0F, 0.0F));
    const float cumulus = saturate(remap(h, 0.0F, 0.1F, 0.0F, 1.0F)) *
                          saturate(remap(h, 0.6F, 0.95F, 1.0F, 0.0F));
    return type < 0.5F ? mix(stratus, strato, type * 2.0F)
                       : mix(strato, cumulus, type * 2.0F - 1.0F);
}

glm::vec3 integrate_step(const glm::vec3& s, float sigma_t, float dt, float& trans) {
    const float step_trans = std::exp(-sigma_t * dt);
    const glm::vec3 contribution = sigma_t > 1e-6F ? (s - s * step_trans) / sigma_t : s * dt;
    const glm::vec3 out = trans * contribution;
    trans *= step_trans;
    return out;
}

glm::vec3 light_scattering(const CloudLayerGpu& l, const glm::vec3& irradiance, float sigma_s,
                           float tau, float cos_theta) {
    glm::vec3 s(0.0F);
    float a = 1.0F;
    float b = 1.0F;
    float c = 1.0F;
    const int octaves = static_cast<int>(l.octaves.w);
    for (int i = 0; i < 4; ++i) {
        if (i >= octaves) break;
        s += irradiance * (sigma_s * a * cloud_phase(l, cos_theta, c) * std::exp(-tau * b));
        a *= l.octaves.x;
        b *= l.octaves.y;
        c *= l.octaves.z;
    }
    return s;
}

namespace {

[[nodiscard]] glm::vec3 weather(const CloudLayerGpu& l, const cloud_noise::Volumes& n,
                                glm::vec2 xz) {
    const glm::vec2 p = xz + glm::vec2(l.offset.x, l.offset.y);
    const glm::vec4 w0 = cloud_noise::sample2d(n.weather, cloud_noise::k_weather_size, p * l.scale.z);
    const glm::vec2 q =
        glm::vec2(0.8F * p.x - 0.6F * p.y, 0.6F * p.x + 0.8F * p.y) * (l.scale.z * 2.71F);
    const glm::vec4 w1 = cloud_noise::sample2d(n.weather, cloud_noise::k_weather_size, q + 0.37F);
    const float field = w0.r * 0.7F + w1.r * 0.3F - 0.5F;
    const float global = l.shell.z;
    const float spread = 2.2F * std::min(global, 1.0F - global) + 0.15F;
    const float coverage = saturate(global + field * spread);
    const float type = saturate(l.shell.w + (w0.g - 0.5F) * 0.5F);
    const float precip = saturate(l.medium.w * (0.4F + 1.2F * w1.b));
    return {coverage, type, precip};
}

} // namespace

float density(const CloudLayerGpu& l, const cloud_noise::Volumes& n, const glm::vec3& p,
              bool cheap) {
    using cloud_noise::remap;
    const float h = (p.y - l.shell.x) / l.shell.y;
    if (h <= 0.0F || h >= 1.0F) return 0.0F;
    const glm::vec3 w = weather(l, n, glm::vec2(p.x, p.z));
    if (w.x <= 0.001F) return 0.0F;
    const float profile = height_profile(h, w.y);
    if (profile <= 0.0F) return 0.0F;
    const glm::vec3 anchored(p.x + l.offset.x, p.y, p.z + l.offset.y);
    const glm::vec4 b = cloud_noise::sample3d(n.base, cloud_noise::k_base_size, anchored * l.scale.x);
    const float low_fbm = b.g * 0.625F + b.b * 0.25F + b.a * 0.125F;
    const float shape =
        saturate(remap(saturate(remap(b.r, low_fbm - 1.0F, 1.0F, 0.0F, 1.0F)),
                       cloud_noise::k_shape_lo, cloud_noise::k_shape_hi, 0.0F, 1.0F)) *
        profile;
    float d = saturate(remap(shape, 1.0F - w.x, 1.0F, 0.0F, 1.0F)) * w.x;
    if (d <= 0.0F || cheap) return d * (1.0F + 2.0F * w.z);
    const glm::vec4 cu = cloud_noise::sample2d(n.curl, cloud_noise::k_curl_size,
                                               glm::vec2(anchored.x, anchored.z) * l.scale.x * 4.0F);
    const glm::vec2 c = glm::vec2(cu.r, cu.g) * 2.0F - 1.0F;
    glm::vec3 q(anchored.x + l.offset.z, p.y, anchored.z + l.offset.w);
    q.x += c.x * l.scale.w * (1.0F - h);
    q.z += c.y * l.scale.w * (1.0F - h);
    const glm::vec4 dn = cloud_noise::sample3d(n.detail, cloud_noise::k_detail_size, q * l.scale.y);
    const float high_fbm = dn.r * 0.625F + dn.g * 0.25F + dn.b * 0.125F;
    const float erode = mix(high_fbm, 1.0F - high_fbm, saturate(h * 6.0F));
    d = saturate(remap(d, erode * l.medium.z, 1.0F, 0.0F, 1.0F));
    return d * (1.0F + 2.0F * w.z);
}

float shadow_transmittance(const CloudLayerGpu& l, const cloud_noise::Volumes& n,
                           const glm::vec3& offset_km, const glm::vec3& sun_dir, int steps) {
    if (l.shell.z <= 0.001F) return 1.0F;
    const float y0 = offset_km.y;
    const float bottom = l.shell.x;
    const float top = bottom + l.shell.y;
    if (sun_dir.y <= 1e-3F) return 1.0F;
    const float t_in = std::max((bottom - y0) / sun_dir.y, 0.0F);
    const float t_out = (top - y0) / sun_dir.y;
    constexpr float k_path_km = 80.0F;
    const float len = std::min(t_out - t_in, k_path_km);
    if (len <= 0.0F) return 1.0F;
    const float dt = len / static_cast<float>(steps);
    float depth = 0.0F;
    for (int i = 0; i < steps; ++i) {
        const glm::vec3 p = offset_km + sun_dir * (t_in + (static_cast<float>(i) + 0.5F) * dt);
        depth += density(l, n, p, false) * dt;
    }
    const float tau = depth * l.medium.x;
    float t = 0.0F;
    float a = 1.0F;
    float b = 1.0F;
    float norm = 0.0F;
    const int octaves = static_cast<int>(l.octaves.w);
    for (int i = 0; i < 4; ++i) {
        if (i >= octaves) break;
        t += a * std::exp(-tau * b);
        norm += a;
        a *= l.octaves.x;
        b *= l.octaves.y;
    }
    return t / std::max(norm, 1e-6F);
}

} // namespace cloud_reference

} // namespace engine::renderer
