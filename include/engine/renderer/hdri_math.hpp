#pragma once

// CPU mirror of the HDRI shader math (ROADMAP R21): the equirectangular
// mapping and rotation (assets/shaders/lib/equirect.slang), the cube face
// directions (lib/ibl_common.slang) and the SH9 basis (lib/sh9.slang). The
// sun analysis and the CPU references of the tests use these, so they must
// stay identical to the shaders.

#include <array>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace engine::renderer {

inline constexpr float k_hdri_pi = 3.14159265358979F;

// Panorama uv of a unit direction (u = 0.5 looks along -Z, v = 0 is +Y).
[[nodiscard]] inline glm::vec2 equirect_uv(const glm::vec3& d) {
    const float u = 0.5F + std::atan2(d.x, -d.z) / (2.0F * k_hdri_pi);
    const float v = std::acos(glm::clamp(d.y, -1.0F, 1.0F)) / k_hdri_pi;
    return {u, v};
}

// Inverse of equirect_uv.
[[nodiscard]] inline glm::vec3 equirect_direction(const glm::vec2& uv) {
    const float phi = (uv.x - 0.5F) * 2.0F * k_hdri_pi;
    const float theta = uv.y * k_hdri_pi;
    return {std::sin(phi) * std::sin(theta), std::cos(theta), -std::cos(phi) * std::sin(theta)};
}

// World direction -> panorama space for an environment rotated by
// `rotation_rad` about +Y: R(-rotation) d.
[[nodiscard]] inline glm::vec3 world_to_panorama(const glm::vec3& d, float rotation_rad) {
    const float c = std::cos(rotation_rad);
    const float s = std::sin(rotation_rad);
    return {c * d.x - s * d.z, d.y, s * d.x + c * d.z};
}

// Panorama -> world space: R(rotation) d.
[[nodiscard]] inline glm::vec3 panorama_to_world(const glm::vec3& d, float rotation_rad) {
    return world_to_panorama(d, -rotation_rad);
}

[[nodiscard]] inline float luminance(const glm::vec3& c) {
    return glm::dot(c, glm::vec3(0.2126F, 0.7152F, 0.0722F));
}

// Unit direction of cube face `face` (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z)
// at face uv in [0, 1] (v down), the Vulkan cube layout.
[[nodiscard]] inline glm::vec3 cube_direction(std::uint32_t face, const glm::vec2& uv) {
    const float s = 2.0F * uv.x - 1.0F;
    const float t = 2.0F * uv.y - 1.0F;
    glm::vec3 d;
    switch (face) {
    case 0: d = {1.0F, -t, -s}; break;
    case 1: d = {-1.0F, -t, s}; break;
    case 2: d = {s, 1.0F, t}; break;
    case 3: d = {s, -1.0F, -t}; break;
    case 4: d = {s, -t, 1.0F}; break;
    default: d = {-s, -t, -1.0F}; break;
    }
    return glm::normalize(d);
}

// The nine real SH basis functions at unit direction `d` (sh9.slang order).
[[nodiscard]] inline std::array<float, 9> sh9_basis(const glm::vec3& d) {
    return {0.282095F,
            0.488603F * d.y,
            0.488603F * d.z,
            0.488603F * d.x,
            1.092548F * d.x * d.y,
            1.092548F * d.y * d.z,
            0.315392F * (3.0F * d.z * d.z - 1.0F),
            1.092548F * d.x * d.z,
            0.546274F * (d.x * d.x - d.y * d.y)};
}

// A_l / pi of coefficient k (cosine-lobe convolution, E / pi convention).
[[nodiscard]] inline float sh9_band_weight(std::uint32_t k) {
    return k == 0 ? 1.0F : (k < 4 ? 2.0F / 3.0F : 0.25F);
}

// Solid angle of row `y` texels of a `width` x `height` panorama.
[[nodiscard]] inline float equirect_texel_solid_angle(std::uint32_t y, std::uint32_t width,
                                                      std::uint32_t height) {
    const float theta = (static_cast<float>(y) + 0.5F) * k_hdri_pi / static_cast<float>(height);
    return (2.0F * k_hdri_pi / static_cast<float>(width)) * (k_hdri_pi / static_cast<float>(height)) *
           std::sin(theta);
}

} // namespace engine::renderer
