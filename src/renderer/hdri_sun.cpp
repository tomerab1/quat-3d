#include "engine/renderer/hdri_sun.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "engine/asset/hdr_decode.hpp"
#include "engine/renderer/hdri_math.hpp"

namespace engine::renderer {

namespace {

constexpr float k_ring_inner_deg = 6.0F;
constexpr float k_ring_outer_deg = 10.0F;

[[nodiscard]] glm::vec3 texel_rgb(std::span<const float> rgb, std::size_t i) {
    const float* p = &rgb[i * 3];
    return {std::max(p[0], 0.0F), std::max(p[1], 0.0F), std::max(p[2], 0.0F)};
}

[[nodiscard]] glm::vec3 texel_direction(std::uint32_t x, std::uint32_t y, std::uint32_t w,
                                        std::uint32_t h) {
    return equirect_direction({(static_cast<float>(x) + 0.5F) / static_cast<float>(w),
                               (static_cast<float>(y) + 0.5F) / static_cast<float>(h)});
}

// Rows whose texels can lie within `deg` of `d` (the band of latitudes
// around d's polar angle), so the searches skip the rest of the image.
struct RowRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

[[nodiscard]] RowRange rows_near(const glm::vec3& d, float deg, std::uint32_t h) {
    const float theta = std::acos(glm::clamp(d.y, -1.0F, 1.0F));
    const float margin = glm::radians(deg) + k_hdri_pi / static_cast<float>(h);
    const float fh = static_cast<float>(h);
    const float lo = std::floor((theta - margin) / k_hdri_pi * fh);
    const float hi = std::ceil((theta + margin) / k_hdri_pi * fh);
    return {static_cast<std::uint32_t>(std::clamp(lo, 0.0F, fh)),
            static_cast<std::uint32_t>(std::clamp(hi, 0.0F, fh))};
}

} // namespace

HdriSun analyze_hdri_sun(const asset::HdrImage& image) {
    HdriSun sun;
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    const std::span<const float> rgb = image.rgb();
    if (w == 0 || h == 0 || rgb.size() < static_cast<std::size_t>(w) * h * 3) return sun;

    // Peak texel (non-finite values from a hostile file read as 0).
    std::size_t peak = 0;
    float peak_lum = -1.0F;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        const float l = luminance(texel_rgb(rgb, i));
        if (std::isfinite(l) && l > peak_lum) {
            peak_lum = l;
            peak = i;
        }
    }
    if (peak_lum <= 0.0F) return sun;
    const glm::vec3 seed = texel_direction(static_cast<std::uint32_t>(peak % w),
                                           static_cast<std::uint32_t>(peak / w), w, h);

    // Sky level: median luminance of the ring around the peak.
    const float ring_in = std::cos(glm::radians(k_ring_inner_deg));
    const float ring_out = std::cos(glm::radians(k_ring_outer_deg));
    std::vector<float> ring;
    const RowRange ring_rows = rows_near(seed, k_ring_outer_deg, h);
    for (std::uint32_t y = ring_rows.begin; y < ring_rows.end; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const float c = glm::dot(texel_direction(x, y, w, h), seed);
            if (c <= ring_in && c >= ring_out) {
                const float l = luminance(texel_rgb(rgb, static_cast<std::size_t>(y) * w + x));
                if (std::isfinite(l)) ring.push_back(l);
            }
        }
    }
    if (ring.empty()) return sun;
    const auto mid = ring.begin() + static_cast<std::ptrdiff_t>(ring.size() / 2);
    std::nth_element(ring.begin(), mid, ring.end());
    sun.sky_luminance = *mid;
    if (peak_lum < HdriSun::k_min_contrast * std::max(sun.sky_luminance, 1e-6F)) return sun;

    // Clamp the cone around the peak; what is clamped off is the sun.
    const float cone = std::cos(glm::radians(HdriSun::k_cone_deg));
    const float clamp = HdriSun::k_clamp_factor * std::max(sun.sky_luminance, 1e-6F);
    glm::vec3 energy{0.0F};
    glm::vec3 centroid{0.0F};
    const RowRange cone_rows = rows_near(seed, HdriSun::k_cone_deg, h);
    for (std::uint32_t y = cone_rows.begin; y < cone_rows.end; ++y) {
        const float dw = equirect_texel_solid_angle(y, w, h);
        for (std::uint32_t x = 0; x < w; ++x) {
            const glm::vec3 d = texel_direction(x, y, w, h);
            if (glm::dot(d, seed) <= cone) continue;
            const glm::vec3 c = texel_rgb(rgb, static_cast<std::size_t>(y) * w + x);
            const float l = luminance(c);
            if (!std::isfinite(l) || l <= clamp) continue;
            const glm::vec3 removed = c * (1.0F - clamp / l) * dw;
            energy += removed;
            centroid += d * luminance(removed);
        }
    }
    if (luminance(energy) <= 0.0F || glm::length(centroid) <= 0.0F) return sun;
    sun.found = true;
    sun.direction = glm::normalize(centroid);
    sun.irradiance = energy;
    sun.cone_cos = cone;
    sun.clamp_luminance = clamp;
    return sun;
}

} // namespace engine::renderer
