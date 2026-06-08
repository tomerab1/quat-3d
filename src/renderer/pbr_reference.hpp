#pragma once

// CPU reference for the direct PBR BRDF in lighting.slang (Phase 4, Slice 4.5).
//
// Private renderer header (not in include/): the single source of truth for the
// Cook-Torrance math, used by the lighting and tonemap full-chain self-tests so
// their expected pixel stays in lock-step with the shader. Mirrors lighting.slang
// exactly — keep the two in sync.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace engine::renderer::pbr_ref {

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

} // namespace engine::renderer::pbr_ref
