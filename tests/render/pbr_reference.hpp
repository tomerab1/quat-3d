#pragma once

// CPU references for the render tests: the direct PBR BRDF in lighting.slang
// (Phase 4, Slice 4.5) and the ACES + sRGB curve in tonemap.slang.
//
// The single source of truth for that math on the test side, used by the
// lighting, tonemap and clustered-lighting tests so their expected pixels stay
// in lock-step with the shaders. Mirrors the shaders exactly — keep them in
// sync.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace quat::test::pbr_ref {

inline constexpr float k_pi = 3.14159265358979F;

inline float d_ggx(float ndoth, float a) {
    const float a2 = a * a;
    const float d = ndoth * ndoth * (a2 - 1.0F) + 1.0F;
    return a2 / (k_pi * d * d);
}

inline float v_smith(float ndotv, float ndotl, float a) {
    const float a2 = a * a;
    const float gv = ndotl * std::sqrt(ndotv * ndotv * (1.0F - a2) + a2);
    const float gl = ndotv * std::sqrt(ndotl * ndotl * (1.0F - a2) + a2);
    return 0.5F / std::max(gv + gl, 1e-5F);
}

inline glm::vec3 f_schlick(float vdoth, glm::vec3 f0) {
    return f0 + (glm::vec3(1.0F) - f0) * std::pow(1.0F - vdoth, 5.0F);
}

// Direct radiance from one directional light, matching lighting.slang's BRDF.
// N, V, L are unit vectors; `intensity` folds into the light colour.
inline glm::vec3 direct(glm::vec3 albedo, float metallic, float roughness, glm::vec3 N,
                        glm::vec3 V, glm::vec3 L, glm::vec3 light_color, float intensity) {
    const glm::vec3 H = glm::normalize(V + L);
    const float ndotl = std::max(glm::dot(N, L), 0.0F);
    const float ndotv = std::max(glm::dot(N, V), 1e-4F);
    const float ndoth = std::max(glm::dot(N, H), 0.0F);
    const float vdoth = std::max(glm::dot(V, H), 0.0F);

    const float a = std::max(roughness * roughness, 1e-3F);
    const glm::vec3 f0 = glm::mix(glm::vec3(0.04F), albedo, metallic);

    const float D = d_ggx(ndoth, a);
    const float Vis = v_smith(ndotv, ndotl, a);
    const glm::vec3 F = f_schlick(vdoth, f0);

    const glm::vec3 spec = D * Vis * F;
    const glm::vec3 kd = (glm::vec3(1.0F) - F) * (1.0F - metallic);
    const glm::vec3 diffuse = kd * albedo / k_pi;

    return (diffuse + spec) * light_color * intensity * ndotl;
}

// ACES filmic fit (Narkowicz), as tonemap.slang applies it.
inline glm::vec3 aces(glm::vec3 x) {
    const float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
    const glm::vec3 num = x * (a * x + b);
    const glm::vec3 den = x * (c * x + d) + e;
    return glm::clamp(num / den, glm::vec3(0.0F), glm::vec3(1.0F));
}

// Linear -> sRGB transfer function.
inline float linear_to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F
                           : 1.055F * std::pow(std::max(c, 0.0F), 1.0F / 2.4F) - 0.055F;
}

} // namespace quat::test::pbr_ref
