#pragma once

// CPU mirror of the cloud noise (ROADMAP R13.4; lib/cloud_noise.slang).
//
// The volumetric clouds read four baked, tileable textures (Schneider's
// Nubis layout, see cloud_noise.slang): the 128³ base volume
// (Perlin-Worley + Worley fBm), the 32³ detail volume, a 128² curl-noise
// texture and a 512² weather-noise texture. CloudNoisePass bakes them on
// the GPU; these functions are the reference they are tested against, and
// bake_cloud_noise_cpu() + the samplers below let CPU references of the
// cloud density (cloud shadows, the march) read the same quantised texels
// with the same REPEAT trilinear filtering the GPU uses.
//
// Integer hashing (pcg3d) makes the lattices identical on both sides; the
// float interpolation may differ by rounding only.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace engine::renderer::cloud_noise {

inline constexpr std::uint32_t k_base_size = 128;   // base volume edge (texels)
inline constexpr std::uint32_t k_detail_size = 32;  // detail volume edge
inline constexpr std::uint32_t k_curl_size = 128;   // curl texture edge
inline constexpr std::uint32_t k_weather_size = 512; // weather texture edge

inline constexpr std::uint32_t k_seed_perlin = 3;
inline constexpr std::uint32_t k_seed_worley = 11;
inline constexpr std::uint32_t k_seed_detail = 29;
inline constexpr std::uint32_t k_seed_curl = 41;
inline constexpr std::uint32_t k_seed_weather = 53;

// CLOUD_WEATHER_LO / HI and CLOUD_SHAPE_LO / HI of lib/cloud_noise.slang:
// the measured ranges stretched to [0, 1].
inline constexpr float k_weather_lo = 0.55F;
inline constexpr float k_weather_hi = 0.92F;
inline constexpr float k_shape_lo = 0.70F;
inline constexpr float k_shape_hi = 0.93F;

[[nodiscard]] glm::uvec3 pcg3d(glm::uvec3 v);
[[nodiscard]] glm::vec3 hash33(glm::uvec3 c, std::uint32_t seed);
[[nodiscard]] float gradient_noise(glm::vec3 p, int period, std::uint32_t seed);
[[nodiscard]] float worley_noise(glm::vec3 p, int period, std::uint32_t seed);
[[nodiscard]] float remap(float v, float lo, float hi, float new_lo, float new_hi);
[[nodiscard]] float worley_fbm(glm::vec3 uvw, int freq, std::uint32_t seed);
[[nodiscard]] float perlin_fbm(glm::vec3 uvw, int freq, int octaves, std::uint32_t seed);

// The texel functions the bake evaluates at texel centres (uvw in [0, 1)).
[[nodiscard]] glm::vec4 base_noise(glm::vec3 uvw);
[[nodiscard]] glm::vec4 detail_noise(glm::vec3 uvw);
[[nodiscard]] glm::vec2 curl_noise(glm::vec2 uv);
[[nodiscard]] glm::vec4 weather_noise(glm::vec2 uv);

// UNORM8 quantisation as the GPU stores a float write (round to nearest).
[[nodiscard]] std::uint8_t to_unorm8(float v);

// Baked textures, texel-for-texel what CloudNoisePass writes (RGBA8 UNORM;
// curl stored as RG in an RGBA texel, ba = 0, 1).
struct Volumes {
    std::vector<glm::u8vec4> base;    // k_base_size³, x fastest
    std::vector<glm::u8vec4> detail;  // k_detail_size³
    std::vector<glm::u8vec4> curl;    // k_curl_size²
    std::vector<glm::u8vec4> weather; // k_weather_size²
};

// Bakes every texture on the CPU (multi-threaded; ~1 s in release). With
// `base_only_slices` > 0 only that many z slices of the base volume are
// baked (tests that look at a few texels), the rest left zero.
[[nodiscard]] Volumes bake_cpu(std::uint32_t base_only_slices = 0);

// Trilinear / bilinear REPEAT sampling of a baked texture at normalised
// coordinates, as a Vulkan linear sampler with REPEAT addressing filters it
// (texel centres at (i + 0.5) / size). Returns values in [0, 1].
[[nodiscard]] glm::vec4 sample3d(const std::vector<glm::u8vec4>& texels, std::uint32_t size,
                                 glm::vec3 uvw);
[[nodiscard]] glm::vec4 sample2d(const std::vector<glm::u8vec4>& texels, std::uint32_t size,
                                 glm::vec2 uv);

} // namespace engine::renderer::cloud_noise
