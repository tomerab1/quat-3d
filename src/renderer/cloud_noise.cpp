// CPU mirror of lib/cloud_noise.slang — see include/engine/renderer/cloud_noise.hpp.
// Every function follows its Slang twin line by line (same operation order),
// so the two agree to float rounding.

#include "engine/renderer/cloud_noise.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace engine::renderer::cloud_noise {

namespace {

[[nodiscard]] glm::uvec3 wrap3(glm::ivec3 c, int period) {
    return glm::uvec3(((c % period) + period) % period);
}

[[nodiscard]] glm::vec3 fade3(glm::vec3 t) {
    return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

[[nodiscard]] glm::uvec3 seeded(glm::uvec3 c, std::uint32_t seed) {
    return c + glm::uvec3(seed, seed * 7U, seed * 13U);
}

[[nodiscard]] float corner_dot(glm::uvec3 c, std::uint32_t seed, glm::vec3 d) {
    const std::uint32_t h = pcg3d(seeded(c, seed)).x % 12U;
    const float u = h < 8U ? d.x : d.y;
    const float v = h < 4U ? d.y : d.z;
    return ((h & 1U) == 0U ? u : -u) + ((h & 2U) == 0U ? v : -v);
}

[[nodiscard]] float lerp(float a, float b, float t) { return a + (b - a) * t; }

[[nodiscard]] float saturate(float v) { return std::clamp(v, 0.0F, 1.0F); }

// Runs fn(z) for z in [0, count) on the hardware threads.
template <typename Fn> void parallel_slices(std::uint32_t count, Fn fn) {
    const std::uint32_t workers =
        std::clamp<std::uint32_t>(std::thread::hardware_concurrency(), 1U, 16U);
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::uint32_t w = 0; w < workers; ++w) {
        threads.emplace_back([w, workers, count, &fn] {
            for (std::uint32_t z = w; z < count; z += workers) fn(z);
        });
    }
    for (std::thread& t : threads) t.join();
}

[[nodiscard]] glm::u8vec4 quantise(glm::vec4 v) {
    return {to_unorm8(v.x), to_unorm8(v.y), to_unorm8(v.z), to_unorm8(v.w)};
}

[[nodiscard]] glm::vec4 texel(const std::vector<glm::u8vec4>& t, std::size_t i) {
    return glm::vec4(t[i]) * (1.0F / 255.0F);
}

} // namespace

glm::uvec3 pcg3d(glm::uvec3 v) {
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

glm::vec3 hash33(glm::uvec3 c, std::uint32_t seed) {
    const glm::uvec3 h = pcg3d(seeded(c, seed));
    return glm::vec3(h >> 8U) * (1.0F / 16777216.0F);
}

float gradient_noise(glm::vec3 p, int period, std::uint32_t seed) {
    const glm::vec3 fl = glm::floor(p);
    const glm::ivec3 i(fl);
    const glm::vec3 f = p - fl;
    const glm::vec3 u = fade3(f);
    auto n = [&](int x, int y, int z) {
        return corner_dot(wrap3(i + glm::ivec3(x, y, z), period), seed,
                          f - glm::vec3(static_cast<float>(x), static_cast<float>(y),
                                        static_cast<float>(z)));
    };
    const float x00 = lerp(n(0, 0, 0), n(1, 0, 0), u.x);
    const float x10 = lerp(n(0, 1, 0), n(1, 1, 0), u.x);
    const float x01 = lerp(n(0, 0, 1), n(1, 0, 1), u.x);
    const float x11 = lerp(n(0, 1, 1), n(1, 1, 1), u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

float worley_noise(glm::vec3 p, int period, std::uint32_t seed) {
    const glm::vec3 fl = glm::floor(p);
    const glm::ivec3 i(fl);
    const glm::vec3 f = p - fl;
    float best = 1.0e9F;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const glm::ivec3 o(x, y, z);
                const glm::vec3 feature = glm::vec3(o) + hash33(wrap3(i + o, period), seed);
                const glm::vec3 d = feature - f;
                best = std::min(best, glm::dot(d, d));
            }
        }
    }
    return 1.0F - saturate(std::sqrt(best));
}

float remap(float v, float lo, float hi, float new_lo, float new_hi) {
    return new_lo + (v - lo) * (new_hi - new_lo) / (hi - lo);
}

float worley_fbm(glm::vec3 uvw, int freq, std::uint32_t seed) {
    return worley_noise(uvw * static_cast<float>(freq), freq, seed) * 0.625F +
           worley_noise(uvw * static_cast<float>(freq * 2), freq * 2, seed) * 0.25F +
           worley_noise(uvw * static_cast<float>(freq * 4), freq * 4, seed) * 0.125F;
}

float perlin_fbm(glm::vec3 uvw, int freq, int octaves, std::uint32_t seed) {
    float sum = 0.0F;
    float amp = 0.5F;
    float norm = 0.0F;
    int f = freq;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * gradient_noise(uvw * static_cast<float>(f), f,
                                    seed + static_cast<std::uint32_t>(o));
        norm += amp;
        amp *= 0.5F;
        f *= 2;
    }
    return saturate(sum / norm * 0.5F + 0.5F);
}

glm::vec4 base_noise(glm::vec3 uvw) {
    const float perlin = perlin_fbm(uvw, 4, 5, k_seed_perlin);
    const float worley = worley_fbm(uvw, 4, k_seed_worley);
    const float perlin_worley = saturate(remap(perlin, 0.0F, 1.0F, worley, 1.0F));
    return {perlin_worley, worley_fbm(uvw, 8, k_seed_worley + 1U),
            worley_fbm(uvw, 16, k_seed_worley + 2U), worley_fbm(uvw, 32, k_seed_worley + 3U)};
}

glm::vec4 detail_noise(glm::vec3 uvw) {
    return {worley_fbm(uvw, 2, k_seed_detail), worley_fbm(uvw, 4, k_seed_detail + 1U),
            worley_fbm(uvw, 8, k_seed_detail + 2U), 1.0F};
}

glm::vec2 curl_noise(glm::vec2 uv) {
    constexpr int freq = 4;
    constexpr float h = 1.0F / 512.0F;
    const glm::vec3 p = glm::vec3(uv, 0.0F) * static_cast<float>(freq);
    const float e = h * static_cast<float>(freq);
    auto g = [&](glm::vec3 q, std::uint32_t s) { return gradient_noise(q, freq, k_seed_curl + s); };
    const float d3dy = g(p + glm::vec3(0, e, 0), 2) - g(p - glm::vec3(0, e, 0), 2);
    const float d2dz = g(p + glm::vec3(0, 0, e), 1) - g(p - glm::vec3(0, 0, e), 1);
    const float d1dz = g(p + glm::vec3(0, 0, e), 0) - g(p - glm::vec3(0, 0, e), 0);
    const float d3dx = g(p + glm::vec3(e, 0, 0), 2) - g(p - glm::vec3(e, 0, 0), 2);
    const glm::vec2 curl = glm::vec2(d3dy - d2dz, d1dz - d3dx) / (2.0F * e);
    return glm::clamp(curl * 0.25F + 0.5F, 0.0F, 1.0F);
}

glm::vec4 weather_noise(glm::vec2 uv) {
    const glm::vec3 p(uv, 0.5F);
    const float perlin = perlin_fbm(p, 4, 5, k_seed_weather);
    const float worley = worley_fbm(p, 4, k_seed_weather + 1U);
    const float coverage =
        saturate(remap(saturate(remap(perlin, 0.0F, 1.0F, worley, 1.0F)), k_weather_lo,
                       k_weather_hi, 0.0F, 1.0F));
    return {coverage, perlin_fbm(p, 2, 3, k_seed_weather + 2U),
            perlin_fbm(p, 3, 3, k_seed_weather + 3U), 1.0F};
}

std::uint8_t to_unorm8(float v) {
    return static_cast<std::uint8_t>(std::lround(saturate(v) * 255.0F));
}

Volumes bake_cpu(std::uint32_t base_only_slices) {
    Volumes out;
    const std::uint32_t b = k_base_size;
    out.base.assign(static_cast<std::size_t>(b) * b * b, glm::u8vec4(0));
    const std::uint32_t base_slices = base_only_slices > 0 ? std::min(base_only_slices, b) : b;
    parallel_slices(base_slices, [&](std::uint32_t z) {
        for (std::uint32_t y = 0; y < b; ++y) {
            for (std::uint32_t x = 0; x < b; ++x) {
                const glm::vec3 uvw = (glm::vec3(x, y, z) + 0.5F) / static_cast<float>(b);
                out.base[(static_cast<std::size_t>(z) * b + y) * b + x] = quantise(base_noise(uvw));
            }
        }
    });
    if (base_only_slices > 0) return out;
    const std::uint32_t d = k_detail_size;
    out.detail.assign(static_cast<std::size_t>(d) * d * d, glm::u8vec4(0));
    parallel_slices(d, [&](std::uint32_t z) {
        for (std::uint32_t y = 0; y < d; ++y) {
            for (std::uint32_t x = 0; x < d; ++x) {
                const glm::vec3 uvw = (glm::vec3(x, y, z) + 0.5F) / static_cast<float>(d);
                out.detail[(static_cast<std::size_t>(z) * d + y) * d + x] =
                    quantise(detail_noise(uvw));
            }
        }
    });
    const std::uint32_t c = k_curl_size;
    out.curl.assign(static_cast<std::size_t>(c) * c, glm::u8vec4(0));
    parallel_slices(c, [&](std::uint32_t y) {
        for (std::uint32_t x = 0; x < c; ++x) {
            const glm::vec2 uv = (glm::vec2(x, y) + 0.5F) / static_cast<float>(c);
            out.curl[static_cast<std::size_t>(y) * c + x] =
                quantise(glm::vec4(curl_noise(uv), 0.0F, 1.0F));
        }
    });
    const std::uint32_t w = k_weather_size;
    out.weather.assign(static_cast<std::size_t>(w) * w, glm::u8vec4(0));
    parallel_slices(w, [&](std::uint32_t y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const glm::vec2 uv = (glm::vec2(x, y) + 0.5F) / static_cast<float>(w);
            out.weather[static_cast<std::size_t>(y) * w + x] = quantise(weather_noise(uv));
        }
    });
    return out;
}

glm::vec4 sample3d(const std::vector<glm::u8vec4>& texels, std::uint32_t size, glm::vec3 uvw) {
    const glm::vec3 t = uvw * static_cast<float>(size) - 0.5F;
    const glm::vec3 fl = glm::floor(t);
    const glm::vec3 f = t - fl;
    const glm::ivec3 i0(fl);
    const auto s = static_cast<int>(size);
    auto at = [&](int x, int y, int z) {
        const glm::uvec3 c = wrap3(i0 + glm::ivec3(x, y, z), s);
        return texel(texels, (static_cast<std::size_t>(c.z) * size + c.y) * size + c.x);
    };
    const glm::vec4 x00 = glm::mix(at(0, 0, 0), at(1, 0, 0), f.x);
    const glm::vec4 x10 = glm::mix(at(0, 1, 0), at(1, 1, 0), f.x);
    const glm::vec4 x01 = glm::mix(at(0, 0, 1), at(1, 0, 1), f.x);
    const glm::vec4 x11 = glm::mix(at(0, 1, 1), at(1, 1, 1), f.x);
    return glm::mix(glm::mix(x00, x10, f.y), glm::mix(x01, x11, f.y), f.z);
}

glm::vec4 sample2d(const std::vector<glm::u8vec4>& texels, std::uint32_t size, glm::vec2 uv) {
    const glm::vec2 t = uv * static_cast<float>(size) - 0.5F;
    const glm::vec2 fl = glm::floor(t);
    const glm::vec2 f = t - fl;
    const glm::ivec2 i0(fl);
    const auto s = static_cast<int>(size);
    auto at = [&](int x, int y) {
        const glm::ivec2 c = ((i0 + glm::ivec2(x, y)) % s + s) % s;
        return texel(texels, static_cast<std::size_t>(c.y) * size + static_cast<std::size_t>(c.x));
    };
    return glm::mix(glm::mix(at(0, 0), at(1, 0), f.x), glm::mix(at(0, 1), at(1, 1), f.x), f.y);
}

} // namespace engine::renderer::cloud_noise
