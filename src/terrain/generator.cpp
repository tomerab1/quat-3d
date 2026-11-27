#include "engine/terrain/generator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "parallel_rows.hpp"

namespace engine::terrain {

namespace {

// --------------------------------------------------------------------------
// Deterministic RNG (PCG32) — never std::rand or hardware entropy: a saved
// seed must rebuild the identical world on every platform.
// --------------------------------------------------------------------------
struct Pcg32 {
    std::uint64_t state = 0x853c49e6748fea9bULL;

    explicit Pcg32(std::uint64_t seed) { state = seed * 6364136223846793005ULL + 1442695040888963407ULL; }

    std::uint32_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto xorshifted =
            static_cast<std::uint32_t>(((state >> 18U) ^ state) >> 27U);
        const auto rot = static_cast<std::uint32_t>(state >> 59U);
        return (xorshifted >> rot) | (xorshifted << ((32U - rot) & 31U));
    }

    // Uniform float in [0, 1).
    float next_float() { return static_cast<float>(next() >> 8) * (1.0F / 16777216.0F); }
};

// --------------------------------------------------------------------------
// Seeded gradient (Perlin) noise. The permutation table is shuffled from the
// seed; gradients are 8 fixed unit-ish directions (classic Perlin quality is
// ample for terrain FBM and far cheaper than simplex with analytic grads).
// --------------------------------------------------------------------------
class GradientNoise {
public:
    explicit GradientNoise(const std::array<std::uint8_t, 256>& permutation) {
        for (std::size_t i = 0; i < 256; ++i) {
            perm_[i] = permutation[i];
            perm_[i + 256] = permutation[i];
        }
    }

    // Value in [-1, 1].
    [[nodiscard]] float sample(glm::vec2 p) const {
        const glm::vec2 pf = glm::floor(p);
        const auto xi = static_cast<std::int32_t>(pf.x) & 255;
        const auto zi = static_cast<std::int32_t>(pf.y) & 255;
        const glm::vec2 f = p - pf;
        const glm::vec2 u = f * f * (3.0F - 2.0F * f);

        const float g00 = dot_grad(hash(xi, zi), f);
        const float g10 = dot_grad(hash(xi + 1, zi), f - glm::vec2(1.0F, 0.0F));
        const float g01 = dot_grad(hash(xi, zi + 1), f - glm::vec2(0.0F, 1.0F));
        const float g11 = dot_grad(hash(xi + 1, zi + 1), f - glm::vec2(1.0F, 1.0F));
        // Perlin's gradient dot spans ~[-0.7, 0.7]; rescale toward [-1, 1].
        return glm::mix(glm::mix(g00, g10, u.x), glm::mix(g01, g11, u.x), u.y) * 1.42F;
    }

private:
    [[nodiscard]] std::uint8_t hash(std::int32_t x, std::int32_t z) const {
        return perm_[static_cast<std::size_t>(
            perm_[static_cast<std::size_t>(x & 255)] + (z & 255))];
    }

    [[nodiscard]] static float dot_grad(std::uint8_t h, glm::vec2 d) {
        // 8 gradient directions: axis + diagonal.
        constexpr std::array<glm::vec2, 8> grads{{{1.0F, 0.0F},
                                                  {-1.0F, 0.0F},
                                                  {0.0F, 1.0F},
                                                  {0.0F, -1.0F},
                                                  {0.70710678F, 0.70710678F},
                                                  {-0.70710678F, 0.70710678F},
                                                  {0.70710678F, -0.70710678F},
                                                  {-0.70710678F, -0.70710678F}}};
        return glm::dot(grads[h & 7U], d);
    }

    std::array<std::uint8_t, 512> perm_{};
};

// Standard FBM in [-1, 1]-ish.
[[nodiscard]] float fbm(const GradientNoise& noise, glm::vec2 p, int octaves, float lacunarity,
                        float gain) {
    float value = 0.0F;
    float amplitude = 0.5F;
    float total = 0.0F;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * noise.sample(p);
        total += amplitude;
        p *= lacunarity;
        amplitude *= gain;
    }
    return value / total;
}

// Ridged FBM in [0, 1]: sharp crests, the classic mountain profile.
[[nodiscard]] float ridged_fbm(const GradientNoise& noise, glm::vec2 p, int octaves,
                               float lacunarity, float gain) {
    float value = 0.0F;
    float amplitude = 0.5F;
    float total = 0.0F;
    float weight = 1.0F;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.0F - std::abs(noise.sample(p));
        n *= n * weight;
        weight = glm::clamp(n * 2.0F, 0.0F, 1.0F); // crests spawn finer crests
        value += amplitude * n;
        total += amplitude;
        p *= lacunarity;
        amplitude *= gain;
    }
    return value / total;
}

// One texel of the composed height field, normalised [0, 1]. `p` is the
// noise-space position of a texel on the world texel lattice (world_texel),
// so neighbouring tiles from the same seed evaluate their shared edge at
// identical positions.
//
// assets/shaders/lib/terrain_noise.slang mirrors this function and the noise
// above operation for operation (same association order, glm's mix and
// smoothstep formulas) — keep them in sync; GpuTerrainSynth tests compare.
[[nodiscard]] float compose_height(const GradientNoise& noise, glm::vec2 p,
                                   const TerrainParams& tp) {
    // iq domain warp: offset the sample position by another FBM pair.
    const float wx = fbm(noise, p + glm::vec2(5.2F, 1.3F), 4, tp.lacunarity, tp.gain);
    const float wz = fbm(noise, p + glm::vec2(9.7F, 8.3F), 4, tp.lacunarity, tp.gain);
    const glm::vec2 warped =
        p + tp.warp_strength * tp.base_frequency * glm::vec2(wx, wz);

    // Low-frequency continental mask decides where mountains rise.
    const float continent =
        glm::smoothstep(-0.20F, 0.45F, fbm(noise, warped * 0.35F, 4, tp.lacunarity, tp.gain));

    const float plains = 0.5F + 0.5F * fbm(noise, warped, tp.octaves, tp.lacunarity, tp.gain);
    const float mountains = ridged_fbm(noise, warped * 1.7F, tp.octaves, tp.lacunarity, tp.gain);

    // Plains stay gentle (compressed), mountains use the full range.
    const float h = glm::mix(0.18F + 0.22F * plains, 0.25F + 0.75F * mountains, continent);
    return glm::clamp(h, 0.0F, 1.0F);
}

// --------------------------------------------------------------------------
// Hydraulic erosion — particle droplets (Beyer). Serial by design: droplets
// mutate the shared field, and a fixed order keeps the result deterministic.
// --------------------------------------------------------------------------
// Returns false when `cancel` stopped it (checked every 1024 droplets).
bool erode(std::vector<float>& h, std::uint32_t res, const TerrainParams& tp,
           const std::atomic<bool>* cancel) {
    if (tp.erosion_droplets == 0) return true;
    const auto at = [&](std::int32_t x, std::int32_t z) -> float& {
        return h[static_cast<std::size_t>(z) * res + static_cast<std::size_t>(x)];
    };
    // Droplets vary per world tile but stay fully deterministic.
    const auto tile_mix = static_cast<std::uint64_t>(
                              static_cast<std::uint32_t>(tp.world_tile.x) * 73856093U ^
                              static_cast<std::uint32_t>(tp.world_tile.y) * 19349663U)
                          << 32;
    Pcg32 rng((static_cast<std::uint64_t>(tp.seed) ^ tile_mix) * 0x9e3779b97f4a7c15ULL + 1);
    const auto fres = static_cast<float>(res);

    // Erosion is tile-local, so it must not touch the borders: edges stay pure
    // FBM and therefore match the neighbouring tile exactly (streaming seams).
    // The fade is applied to the deposited/eroded amounts (conserving mass) and
    // must reach zero a full brush radius before the edge — the erode brush
    // spreads 3 cells around the droplet and would otherwise leak onto border
    // texels from inside the band.
    const float fade_band = glm::max(fres * 0.05F, 4.0F);
    const auto edge_fade = [&](glm::vec2 p) {
        const float d = glm::min(glm::min(p.x, p.y),
                                 glm::min(fres - 1.0F - p.x, fres - 1.0F - p.y)) -
                        4.0F;
        return glm::clamp(d / fade_band, 0.0F, 1.0F);
    };

    for (std::uint32_t d = 0; d < tp.erosion_droplets; ++d) {
        if ((d & 1023U) == 0 && cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            return false;
        }
        glm::vec2 pos(rng.next_float() * (fres - 2.0F), rng.next_float() * (fres - 2.0F));
        glm::vec2 dir(0.0F);
        float speed = 1.0F;
        float water = 1.0F;
        float sediment = 0.0F;

        for (std::uint32_t life = 0; life < tp.droplet_lifetime; ++life) {
            const auto xi = static_cast<std::int32_t>(pos.x);
            const auto zi = static_cast<std::int32_t>(pos.y);
            if (xi < 0 || zi < 0 || xi >= static_cast<std::int32_t>(res) - 1 ||
                zi >= static_cast<std::int32_t>(res) - 1) {
                break;
            }
            const float fx = pos.x - static_cast<float>(xi);
            const float fz = pos.y - static_cast<float>(zi);

            // Bilinear height + gradient of the current cell.
            const float h00 = at(xi, zi);
            const float h10 = at(xi + 1, zi);
            const float h01 = at(xi, zi + 1);
            const float h11 = at(xi + 1, zi + 1);
            const glm::vec2 grad((h10 - h00) * (1.0F - fz) + (h11 - h01) * fz,
                                 (h01 - h00) * (1.0F - fx) + (h11 - h10) * fx);
            const float height =
                glm::mix(glm::mix(h00, h10, fx), glm::mix(h01, h11, fx), fz);

            dir = dir * tp.erosion_inertia - grad * (1.0F - tp.erosion_inertia);
            const float len = glm::length(dir);
            if (len < 1e-7F) break; // flat: the droplet dies in a puddle
            dir /= len;
            pos += dir;

            const auto nxi = static_cast<std::int32_t>(pos.x);
            const auto nzi = static_cast<std::int32_t>(pos.y);
            if (nxi < 0 || nzi < 0 || nxi >= static_cast<std::int32_t>(res) - 1 ||
                nzi >= static_cast<std::int32_t>(res) - 1) {
                break;
            }
            const float nfx = pos.x - static_cast<float>(nxi);
            const float nfz = pos.y - static_cast<float>(nzi);
            const float new_height =
                glm::mix(glm::mix(at(nxi, nzi), at(nxi + 1, nzi), nfx),
                         glm::mix(at(nxi, nzi + 1), at(nxi + 1, nzi + 1), nfx), nfz);
            const float delta = new_height - height;

            const float capacity =
                glm::max(-delta, 0.01F) * speed * water * tp.erosion_capacity;
            const float fade = edge_fade(pos);
            if (sediment > capacity || delta > 0.0F) {
                // Deposit: fill the pit when moving uphill, else drop the excess.
                const float deposit = (delta > 0.0F
                                           ? glm::min(delta, sediment)
                                           : (sediment - capacity) * tp.erosion_deposit_rate) *
                                      fade;
                sediment -= deposit;
                at(xi, zi) += deposit * (1.0F - fx) * (1.0F - fz);
                at(xi + 1, zi) += deposit * fx * (1.0F - fz);
                at(xi, zi + 1) += deposit * (1.0F - fx) * fz;
                at(xi + 1, zi + 1) += deposit * fx * fz;
            } else {
                // Erode over a soft brush around the droplet (never deeper than
                // the drop). Single-texel erosion carves spike pits that ADD
                // high-frequency noise; the brush is what makes droplets carve
                // smooth gullies (Beyer).
                const float amount =
                    glm::min((capacity - sediment) * tp.erosion_erode_rate, -delta) * fade;
                sediment += amount;
                constexpr std::int32_t radius = 3;
                float weight_total = 0.0F;
                std::array<float, (2 * radius + 1) * (2 * radius + 1)> weights{};
                std::size_t wi = 0;
                for (std::int32_t bz = -radius; bz <= radius; ++bz) {
                    for (std::int32_t bx = -radius; bx <= radius; ++bx, ++wi) {
                        const std::int32_t cx = xi + bx;
                        const std::int32_t cz = zi + bz;
                        if (cx < 0 || cz < 0 || cx >= static_cast<std::int32_t>(res) ||
                            cz >= static_cast<std::int32_t>(res)) {
                            continue;
                        }
                        const float dist = std::sqrt(static_cast<float>(bx * bx + bz * bz));
                        const float wgt = glm::max(0.0F, static_cast<float>(radius) - dist);
                        weights[wi] = wgt;
                        weight_total += wgt;
                    }
                }
                wi = 0;
                for (std::int32_t bz = -radius; bz <= radius; ++bz) {
                    for (std::int32_t bx = -radius; bx <= radius; ++bx, ++wi) {
                        if (weights[wi] <= 0.0F) continue;
                        at(xi + bx, zi + bz) -= amount * weights[wi] / weight_total;
                    }
                }
            }

            speed = std::sqrt(glm::max(speed * speed + delta * -9.81F * 0.01F, 0.0F));
            water *= 1.0F - tp.erosion_evaporate;
            if (water < 0.01F) break;
        }
    }
    return true;
}

} // namespace

bool grid_erosion_within_limits(const TerrainParams& params) {
    if (params.erosion_model != ErosionModel::grid || params.grid.iterations == 0) return true;
    const std::uint64_t texels = static_cast<std::uint64_t>(params.resolution) * params.resolution;
    return params.resolution <= max_grid_erosion_resolution &&
           params.grid.iterations <= max_grid_erosion_steps &&
           texels * params.grid.iterations <= max_grid_erosion_texel_steps;
}

bool erosion_enabled(const TerrainParams& params) {
    return params.erosion_model == ErosionModel::grid ? params.grid.iterations > 0
                                                      : params.erosion_droplets > 0;
}

bool same_shape(const TerrainParams& a, const TerrainParams& b) {
    const GridErosionParams& ga = a.grid;
    const GridErosionParams& gb = b.grid;
    return a.seed == b.seed && a.resolution == b.resolution && a.tile_size_m == b.tile_size_m &&
           a.height_m == b.height_m && a.octaves == b.octaves && a.lacunarity == b.lacunarity &&
           a.gain == b.gain && a.base_frequency == b.base_frequency &&
           a.warp_strength == b.warp_strength && a.erosion_model == b.erosion_model &&
           a.erosion_droplets == b.erosion_droplets && a.droplet_lifetime == b.droplet_lifetime &&
           a.erosion_inertia == b.erosion_inertia && a.erosion_capacity == b.erosion_capacity &&
           a.erosion_deposit_rate == b.erosion_deposit_rate &&
           a.erosion_erode_rate == b.erosion_erode_rate &&
           a.erosion_evaporate == b.erosion_evaporate && ga.iterations == gb.iterations &&
           ga.time_step == gb.time_step && ga.rain == gb.rain && ga.capacity == gb.capacity &&
           ga.erode_rate == gb.erode_rate && ga.deposit_rate == gb.deposit_rate &&
           ga.evaporate == gb.evaporate && ga.min_tilt == gb.min_tilt &&
           ga.max_depth == gb.max_depth && ga.talus == gb.talus &&
           ga.thermal_rate == gb.thermal_rate;
}

std::array<std::uint8_t, 256> noise_permutation(std::uint32_t seed) {
    std::array<std::uint8_t, 256> perm{};
    std::iota(perm.begin(), perm.end(), std::uint8_t{0});
    Pcg32 rng(seed);
    for (int i = 255; i > 0; --i) {
        const int j = static_cast<int>(rng.next() % static_cast<std::uint32_t>(i + 1));
        std::swap(perm[static_cast<std::size_t>(i)], perm[static_cast<std::size_t>(j)]);
    }
    return perm;
}

float noise_texel_scale(const TerrainParams& params) {
    const std::uint32_t res = glm::max(params.resolution, 2U);
    return params.base_frequency / static_cast<float>(res - 1);
}

glm::ivec2 world_texel(const TerrainParams& params, glm::ivec2 local) {
    const auto span = static_cast<std::int32_t>(glm::max(params.resolution, 2U) - 1);
    return params.world_tile * span + local;
}

float grid_erosion_fade(std::uint32_t x, std::uint32_t z, std::uint32_t resolution) {
    // Same band as the droplet fade: zero for the 4 outermost texels, then a
    // ramp over 5% of the tile (at least 4 texels).
    const auto last = static_cast<std::int32_t>(resolution) - 1;
    const auto xi = static_cast<std::int32_t>(x);
    const auto zi = static_cast<std::int32_t>(z);
    const std::int32_t edge = glm::min(glm::min(xi, zi), glm::min(last - xi, last - zi)) - 4;
    const float band = glm::max(static_cast<float>(resolution) * 0.05F, 4.0F);
    return glm::clamp(static_cast<float>(edge) / band, 0.0F, 1.0F);
}

Heightmap generate(const TerrainParams& params, const std::atomic<bool>* cancel) {
    Heightmap map;
    map.resolution = glm::max(params.resolution, 2U);
    map.tile_size_m = params.tile_size_m;
    map.heights.resize(static_cast<std::size_t>(map.resolution) * map.resolution);

    const GradientNoise noise(noise_permutation(params.seed));
    const std::uint32_t res = map.resolution;
    const float scale = noise_texel_scale(params);

    // FBM is per-texel independent — split rows across the hardware threads.
    detail::parallel_rows(res, [&](std::uint32_t z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const glm::ivec2 g = world_texel(
                params, glm::ivec2(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)));
            const glm::vec2 p(static_cast<float>(g.x) * scale, static_cast<float>(g.y) * scale);
            map.heights[static_cast<std::size_t>(z) * res + x] = compose_height(noise, p, params);
        }
    });

    if (params.erosion_model == ErosionModel::droplets) {
        // Droplets run on the normalised field (parameters are resolution-relative).
        if (!erode(map.heights, res, params, cancel)) return {};
        for (float& v : map.heights) v *= params.height_m;
    } else {
        for (float& v : map.heights) v *= params.height_m;
        if (params.grid.iterations > 0) {
            std::vector<float> eroded = map.heights;
            if (!erode_grid(eroded, res, map.metres_per_texel(), params.grid, cancel)) {
                return {};
            }
            // Tile-local erosion fades out before the border (streaming seams).
            detail::parallel_rows(res, [&](std::uint32_t z) {
                for (std::uint32_t x = 0; x < res; ++x) {
                    const float fade = grid_erosion_fade(x, z, res);
                    if (fade <= 0.0F) continue;
                    const std::size_t i = static_cast<std::size_t>(z) * res + x;
                    map.heights[i] += fade * (eroded[i] - map.heights[i]);
                }
            });
        }
    }

    // Track the range.
    map.min_height = std::numeric_limits<float>::max();
    map.max_height = std::numeric_limits<float>::lowest();
    for (const float v : map.heights) {
        map.min_height = glm::min(map.min_height, v);
        map.max_height = glm::max(map.max_height, v);
    }
    return map;
}

std::future<Heightmap> generate_async(TerrainParams params, CancelToken cancel) {
    return std::async(std::launch::async, [params, cancel = std::move(cancel)] {
        return generate(params, cancel.get());
    });
}

} // namespace engine::terrain
