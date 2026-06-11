#include "engine/terrain/generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    explicit GradientNoise(std::uint32_t seed) {
        std::iota(perm_.begin(), perm_.begin() + 256, std::uint8_t{0});
        Pcg32 rng(seed);
        for (int i = 255; i > 0; --i) {
            const int j = static_cast<int>(rng.next() % static_cast<std::uint32_t>(i + 1));
            std::swap(perm_[static_cast<std::size_t>(i)], perm_[static_cast<std::size_t>(j)]);
        }
        for (int i = 0; i < 256; ++i) {
            perm_[static_cast<std::size_t>(i) + 256] = perm_[static_cast<std::size_t>(i)];
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

// One texel of the composed height field, normalised [0, 1]. `uv` in [0, 1]²
// within the tile; world_tile shifts the noise domain so neighbouring tiles
// from the same seed continue each other seamlessly.
[[nodiscard]] float compose_height(const GradientNoise& noise, glm::vec2 uv,
                                   const TerrainParams& tp) {
    const glm::vec2 p = (glm::vec2(tp.world_tile) + uv) * tp.base_frequency;

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
void erode(std::vector<float>& h, std::uint32_t res, const TerrainParams& tp) {
    if (tp.erosion_droplets == 0) return;
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
}

} // namespace

Heightmap generate(const TerrainParams& params) {
    Heightmap map;
    map.resolution = glm::max(params.resolution, 2U);
    map.tile_size_m = params.tile_size_m;
    map.heights.resize(static_cast<std::size_t>(map.resolution) * map.resolution);

    const GradientNoise noise(params.seed);
    const std::uint32_t res = map.resolution;

    // FBM is per-texel independent — split rows across the hardware threads.
    const std::uint32_t workers =
        glm::clamp(std::thread::hardware_concurrency(), 1U, 16U);
    {
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (std::uint32_t w = 0; w < workers; ++w) {
            threads.emplace_back([&, w] {
                for (std::uint32_t z = w; z < res; z += workers) {
                    for (std::uint32_t x = 0; x < res; ++x) {
                        const glm::vec2 uv(static_cast<float>(x) / static_cast<float>(res - 1),
                                           static_cast<float>(z) / static_cast<float>(res - 1));
                        map.heights[static_cast<std::size_t>(z) * res + x] =
                            compose_height(noise, uv, params);
                    }
                }
            });
        }
    } // jthreads join

    // Erosion runs on the normalised field (parameters are resolution-relative).
    erode(map.heights, res, params);

    // Scale to metres and track the range.
    map.min_height = std::numeric_limits<float>::max();
    map.max_height = std::numeric_limits<float>::lowest();
    for (float& v : map.heights) {
        v *= params.height_m;
        map.min_height = glm::min(map.min_height, v);
        map.max_height = glm::max(map.max_height, v);
    }
    return map;
}

std::future<Heightmap> generate_async(TerrainParams params) {
    return std::async(std::launch::async, [params] { return generate(params); });
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// FNV-1a over the height bits — exact determinism check.
[[nodiscard]] std::uint64_t hash_map(const Heightmap& m) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const float v : m.heights) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        h = (h ^ bits) * 1099511628211ULL;
    }
    return h;
}

[[nodiscard]] float mean_abs_gradient(const Heightmap& m) {
    double total = 0.0;
    const std::uint32_t res = m.resolution;
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x + 1 < res; ++x) {
            total += std::abs(m.at(static_cast<std::int32_t>(x) + 1, static_cast<std::int32_t>(z)) -
                              m.at(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)));
        }
    }
    return static_cast<float>(total / (static_cast<double>(res) * (res - 1)));
}

} // namespace

std::expected<void, core::Error> run_terrain_self_test() {
    TerrainParams tp;
    tp.resolution = 257;
    tp.erosion_droplets = 20'000;

    const Heightmap a = generate(tp);
    if (!a.valid()) return fail("terrain self-test: generated map invalid");
    if (a.min_height < -1.0F || a.max_height > tp.height_m + 1.0F ||
        a.max_height - a.min_height < 0.05F * tp.height_m) {
        return fail("terrain self-test: heights out of bounds or implausibly flat");
    }

    // Determinism: the same params must reproduce the exact same bits.
    const Heightmap b = generate(tp);
    if (hash_map(a) != hash_map(b)) {
        return fail("terrain self-test: same seed produced different maps");
    }

    // Seed sensitivity: a different seed must change the field.
    TerrainParams other = tp;
    other.seed = tp.seed + 1;
    if (hash_map(generate(other)) == hash_map(a)) {
        return fail("terrain self-test: different seed produced an identical map");
    }

    // Domain warp must actually move material around.
    TerrainParams unwarped = tp;
    unwarped.warp_strength = 0.0F;
    unwarped.erosion_droplets = 0;
    TerrainParams warped = unwarped;
    warped.warp_strength = tp.warp_strength;
    if (hash_map(generate(unwarped)) == hash_map(generate(warped))) {
        return fail("terrain self-test: domain warp had no effect");
    }

    // Erosion smooths: the mean local gradient must drop vs the uneroded field,
    // while the height range stays sane (erosion moves material, not mass out).
    TerrainParams uneroded = tp;
    uneroded.erosion_droplets = 0;
    const Heightmap rough = generate(uneroded);
    if (!(mean_abs_gradient(a) < mean_abs_gradient(rough))) {
        return fail("terrain self-test: erosion did not smooth the field");
    }

    // The bilinear/normal helpers behave at the centre of the tile.
    const float centre = a.tile_size_m * 0.5F;
    const glm::vec3 n = a.normal(centre, centre);
    if (!(n.y > 0.0F) || std::abs(glm::length(n) - 1.0F) > 1e-3F) {
        return fail("terrain self-test: normal helper returned a bad normal");
    }
    return {};
}

} // namespace engine::terrain
