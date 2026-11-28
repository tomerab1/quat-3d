// CPU mirror of the cloud layer's density and of cloud_sun_transmittance
// (assets/shaders/lib/atmosphere.slang): the reference the cloud shadow map
// is tested against. Single-precision float throughout, operation for
// operation, so the two agree to rounding (keep them in sync).

#include <algorithm>
#include <cmath>

#include "engine/renderer/cloud_shadow_pass.hpp"

namespace engine::renderer {

namespace {

constexpr float kGroundRadius = 6360.0F; // ATMOS_GROUND_RADIUS (km)
constexpr float kViewHeight = 6360.2F;   // ATMOS_VIEW_HEIGHT (km)
constexpr int   kSteps = 16;             // CLOUD_SHADOW_STEPS
constexpr float kMaxPathKm = 80.0F;      // CLOUD_SHADOW_MAX_PATH_KM

struct Params {
    float coverage, extinction, bottom_km, thickness_km, freq, detail, wind_x, wind_z;
};

[[nodiscard]] float frac(float x) { return x - std::floor(x); }
[[nodiscard]] float saturate(float x) { return std::clamp(x, 0.0F, 1.0F); }
[[nodiscard]] float lerp(float a, float b, float t) { return a + (b - a) * t; }
[[nodiscard]] float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float cloud_hash(glm::vec2 p) {
    glm::vec3 q(frac(p.x * 0.1031F), frac(p.y * 0.1031F), frac(p.x * 0.1031F));
    q += glm::dot(q, glm::vec3(q.y, q.z, q.x) + 33.33F);
    return frac((q.x + q.y) * q.z);
}

[[nodiscard]] float cloud_value_noise(glm::vec2 p) {
    const glm::vec2 i = glm::floor(p);
    const glm::vec2 f = p - i;
    const glm::vec2 u = f * f * (3.0F - 2.0F * f);
    const float a = cloud_hash(i);
    const float b = cloud_hash(i + glm::vec2(1, 0));
    const float c = cloud_hash(i + glm::vec2(0, 1));
    const float d = cloud_hash(i + glm::vec2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

[[nodiscard]] float cloud_fbm(glm::vec2 p) {
    float v = 0.0F;
    float amp = 0.5F;
    for (int i = 0; i < 5; ++i) {
        v += amp * cloud_value_noise(p);
        p = glm::vec2(0.80F * p.x + 0.60F * p.y, -0.60F * p.x + 0.80F * p.y) * 2.17F +
            glm::vec2(13.7F, 9.1F);
        amp *= 0.5F;
    }
    return v;
}

[[nodiscard]] float cloud_density(const glm::vec3& p, float altitude_km, float bottom_r,
                                  float top_r, const Params& cp) {
    const float h01 = saturate((altitude_km - bottom_r) / std::max(top_r - bottom_r, 1e-3F));
    const float profile = smoothstep(0.0F, 0.20F, h01) * (1.0F - smoothstep(0.5F, 1.0F, h01));
    if (profile <= 0.0F) return 0.0F;
    const glm::vec2 wind(cp.wind_x, cp.wind_z);
    const glm::vec2 uv = (glm::vec2(p.x, p.z) + wind) * cp.freq;
    const float base = cloud_fbm(uv);
    const float coverage_t = 1.0F - cp.coverage;
    float d = saturate((base - coverage_t) / std::max(1.0F - coverage_t, 1e-3F));
    const float detail = cloud_fbm(uv * 5.31F + 17.7F + wind * cp.freq * 2.3F);
    d = saturate(d - (1.0F - d) * detail * cp.detail);
    return d * d * (3.0F - 2.0F * d) * profile;
}

[[nodiscard]] float shell_distance(float r, float mu, float shell_radius) {
    const float disc = r * r * (mu * mu - 1.0F) + shell_radius * shell_radius;
    if (disc < 0.0F) return -1.0F;
    return -r * mu + std::sqrt(disc);
}

} // namespace

float cloud_sun_transmittance_reference(const glm::vec3& offset_km, const glm::vec3& sun,
                                        const CloudSettings& clouds) {
    const glm::vec4 a = clouds.pack_a();
    const glm::vec4 b = clouds.pack_b();
    const Params cp{a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w};
    if (cp.coverage <= 0.001F) return 1.0F;
    const float bottom_r = kGroundRadius + cp.bottom_km;
    const float top_r = bottom_r + cp.thickness_km;
    const glm::vec3 p0(offset_km.x, kViewHeight + offset_km.y, offset_km.z);
    const float r = glm::length(p0);
    const float mu = glm::dot(p0, sun) / r;
    const float t_out = shell_distance(r, mu, top_r);
    if (t_out <= 0.0F) return 1.0F;
    const float t_in = r < bottom_r ? std::max(shell_distance(r, mu, bottom_r), 0.0F) : 0.0F;
    const float len = std::min(t_out - t_in, kMaxPathKm);
    if (len <= 0.0F) return 1.0F;
    const float dt = len / static_cast<float>(kSteps);
    float depth = 0.0F;
    for (int i = 0; i < kSteps; ++i) {
        const float t = t_in + (static_cast<float>(i) + 0.5F) * dt;
        const glm::vec3 p = p0 + sun * t;
        const glm::vec3 rel = p - glm::vec3(0.0F, kViewHeight, 0.0F);
        depth += cloud_density(rel, glm::length(p), bottom_r, top_r, cp) * dt;
    }
    return std::exp(-depth * cp.extinction * 0.6F);
}

} // namespace engine::renderer
