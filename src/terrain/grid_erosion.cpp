// Grid erosion — the CPU reference of the GPU synthesis pass's erosion
// (ROADMAP R10.2). Virtual-pipe hydraulic erosion (Mei, Decaudin & Hu 2007)
// with a depth-limited capacity (Stava et al. 2008) and thermal weathering.
//
// Each iteration runs four grid passes, exactly like the four compute
// dispatches in assets/shaders/terrain_synth.slang (erode_flux, erode_water,
// erode_apply, erode_transport): every pass writes only its own cell and
// reads only state that no pass writes concurrently, so the passes split over
// rows (a fixed worker set separated by barriers) and the result does not
// depend on the thread count. Keep the arithmetic in sync with the shader —
// same operations in the same order; GpuTerrainSynth tests compare the two.

#include <array>
#include <barrier>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain/generator.hpp"

namespace engine::terrain {

namespace {

constexpr float k_gravity = 9.81F;
constexpr float k_min_flow_depth = 1e-4F; // m: shallower water has no velocity

// glm::mix's formula (and the shader's mix_ref): x * (1 - a) + y * a.
[[nodiscard]] float mix_ref(float x, float y, float a) { return x * (1.0F - a) + y * a; }

class GridSim {
public:
    GridSim(std::vector<float>& heights, std::uint32_t resolution, const GridErosionStep& step)
        : res_(resolution), last_(static_cast<std::int32_t>(resolution) - 1), k_(step),
          b_(heights), d_(heights.size(), 0.0F),
          sed_{std::vector<float>(heights.size(), 0.0F), std::vector<float>(heights.size(), 0.0F)},
          flux_(heights.size(), glm::vec4(0.0F)), vel_(heights.size(), glm::vec4(0.0F)) {}

    // 1. Outflow flux (L -x, R +x, T -z, B +z) from water-surface differences.
    void flux_row(std::int32_t z) {
        for (std::int32_t x = 0; x <= last_; ++x) {
            const std::size_t i = at(x, z);
            const float d1 = d_[i] + k_.rain_dt;
            const float h = b_[i] + d1;
            const auto out = [&](std::int32_t nx, std::int32_t nz, float f) {
                const std::size_t j = at(nx, nz);
                const float hn = b_[j] + (d_[j] + k_.rain_dt);
                return glm::max(0.0F, f + k_.pipe * (h - hn));
            };
            const glm::vec4 f = flux_[i];
            const glm::vec4 o(x > 0 ? out(x - 1, z, f.x) : 0.0F,
                              x < last_ ? out(x + 1, z, f.y) : 0.0F,
                              z > 0 ? out(x, z - 1, f.z) : 0.0F,
                              z < last_ ? out(x, z + 1, f.w) : 0.0F);
            const float sum = o.x + o.y + o.z + o.w;
            // Never drain more water than the cell holds.
            const float k = sum > 0.0F ? glm::min(1.0F, d1 * k_.area / (sum * k_.dt)) : 1.0F;
            flux_[i] = o * k;
        }
    }

    // 2. Water depth, velocity, slope and thermal transfer.
    void water_row(std::int32_t z) {
        for (std::int32_t x = 0; x <= last_; ++x) {
            const std::size_t i = at(x, z);
            const float d1 = d_[i] + k_.rain_dt;
            const glm::vec4 f = flux_[i];
            const float in_l = x > 0 ? flux_[at(x - 1, z)].y : 0.0F;
            const float in_r = x < last_ ? flux_[at(x + 1, z)].x : 0.0F;
            const float in_t = z > 0 ? flux_[at(x, z - 1)].w : 0.0F;
            const float in_b = z < last_ ? flux_[at(x, z + 1)].z : 0.0F;
            const float in_sum = in_l + in_r + in_t + in_b;
            const float out_sum = f.x + f.y + f.z + f.w;
            const float d2 = glm::max(0.0F, d1 + k_.dt * (in_sum - out_sum) * k_.inv_area);

            const float wx = (in_l - f.x + f.y - in_r) * 0.5F;
            const float wz = (in_t - f.z + f.w - in_b) * 0.5F;
            const float depth = 0.5F * (d1 + d2);
            const bool flows = depth > k_min_flow_depth;
            const float u = flows ? wx / (k_.cell * depth) : 0.0F;
            const float v = flows ? wz / (k_.cell * depth) : 0.0F;

            const std::int32_t xl = glm::max(x - 1, 0);
            const std::int32_t xr = glm::min(x + 1, last_);
            const std::int32_t zt = glm::max(z - 1, 0);
            const std::int32_t zb = glm::min(z + 1, last_);
            const float sx = (xr - xl) == 2 ? 0.5F : 1.0F;
            const float sz = (zb - zt) == 2 ? 0.5F : 1.0F;
            const float gx = (b_[at(xr, z)] - b_[at(xl, z)]) * (sx * k_.inv_cell);
            const float gz = (b_[at(x, zb)] - b_[at(x, zt)]) * (sz * k_.inv_cell);
            const float g2 = gx * gx + gz * gz;
            const float sin_tilt = std::sqrt(g2 / (1.0F + g2));

            // Thermal weathering: a symmetric pairwise transfer of the height
            // difference beyond the talus slope (mass-conserving).
            float td = 0.0F;
            const float bi = b_[i];
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const std::int32_t nx = x + dx;
                    const std::int32_t nz = z + dz;
                    if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 || nx > last_ || nz > last_) {
                        continue;
                    }
                    const float dist = (dx != 0 && dz != 0) ? k_.diagonal : k_.cell;
                    const float dh = b_[at(nx, nz)] - bi;
                    const float excess = std::abs(dh) - k_.talus * dist;
                    if (excess > 0.0F) td += dh > 0.0F ? excess : -excess;
                }
            }
            vel_[i] = glm::vec4(u, v, sin_tilt, td * k_.thermal);
            d_[i] = d2;
        }
    }

    // 3. Erosion / deposition against the transport capacity.
    void apply_row(std::int32_t z, std::uint32_t cur) {
        std::vector<float>& sed = sed_[cur];
        for (std::int32_t x = 0; x <= last_; ++x) {
            const std::size_t i = at(x, z);
            const glm::vec4 w = vel_[i];
            float s = sed[i];
            float h = b_[i];
            const float speed = std::sqrt(w.x * w.x + w.y * w.y);
            const float ramp = glm::clamp(d_[i] * k_.inv_max_depth, 0.0F, 1.0F);
            const float capacity = k_.capacity * glm::max(w.z, k_.min_tilt) * speed * ramp;
            if (capacity > s) {
                const float amount = k_.erode_dt * (capacity - s);
                h -= amount;
                s += amount;
            } else {
                const float amount = k_.deposit_dt * (s - capacity);
                h += amount;
                s -= amount;
            }
            b_[i] = h + w.w;
            sed[i] = s;
        }
    }

    // 4. Semi-Lagrangian sediment transport + evaporation.
    void transport_row(std::int32_t z, std::uint32_t cur) {
        const std::vector<float>& s = sed_[cur];
        std::vector<float>& out = sed_[cur ^ 1U];
        const auto flast = static_cast<float>(last_);
        for (std::int32_t x = 0; x <= last_; ++x) {
            const std::size_t i = at(x, z);
            const glm::vec4 w = vel_[i];
            const float px = glm::clamp(static_cast<float>(x) - w.x * k_.advect, 0.0F, flast);
            const float pz = glm::clamp(static_cast<float>(z) - w.y * k_.advect, 0.0F, flast);
            const auto x0 = static_cast<std::int32_t>(px);
            const auto z0 = static_cast<std::int32_t>(pz);
            const std::int32_t x1 = glm::min(x0 + 1, last_);
            const std::int32_t z1 = glm::min(z0 + 1, last_);
            const float tx = px - static_cast<float>(x0);
            const float tz = pz - static_cast<float>(z0);
            out[i] = mix_ref(mix_ref(s[at(x0, z0)], s[at(x1, z0)], tx),
                             mix_ref(s[at(x0, z1)], s[at(x1, z1)], tx), tz);
            d_[i] *= k_.keep_water;
        }
    }

    // Sediment still in suspension settles where it is.
    void settle_row(std::int32_t z, std::uint32_t cur) {
        for (std::int32_t x = 0; x <= last_; ++x) b_[at(x, z)] += sed_[cur][at(x, z)];
    }

private:
    [[nodiscard]] std::size_t at(std::int32_t x, std::int32_t z) const {
        return static_cast<std::size_t>(z) * res_ + static_cast<std::size_t>(x);
    }

    std::uint32_t                     res_;
    std::int32_t                      last_;
    GridErosionStep                   k_;
    std::vector<float>&               b_;
    std::vector<float>                d_;
    std::array<std::vector<float>, 2> sed_;
    std::vector<glm::vec4>            flux_;
    std::vector<glm::vec4>            vel_;
};

} // namespace

GridErosionStep grid_erosion_step(const GridErosionParams& p, float cell_m) {
    GridErosionStep k;
    k.dt = p.time_step;
    k.rain_dt = p.rain * p.time_step;
    k.pipe = p.time_step * k_gravity * cell_m;
    k.area = cell_m * cell_m;
    k.inv_area = 1.0F / k.area;
    k.cell = cell_m;
    k.inv_cell = 1.0F / cell_m;
    k.diagonal = cell_m * 1.41421356F;
    k.advect = p.time_step / cell_m;
    k.erode_dt = p.erode_rate * p.time_step;
    k.deposit_dt = p.deposit_rate * p.time_step;
    k.keep_water = 1.0F - p.evaporate * p.time_step;
    k.thermal = p.thermal_rate * 0.125F;
    k.inv_max_depth = 1.0F / glm::max(p.max_depth, 1e-6F);
    k.capacity = p.capacity;
    k.min_tilt = p.min_tilt;
    k.talus = p.talus;
    return k;
}

void erode_grid(std::vector<float>& heights_m, std::uint32_t resolution, float cell_m,
                const GridErosionParams& params) {
    if (params.iterations == 0 || resolution < 3 || !(cell_m > 0.0F) ||
        heights_m.size() != static_cast<std::size_t>(resolution) * resolution) {
        return;
    }
    GridSim sim(heights_m, resolution, grid_erosion_step(params, cell_m));
    const auto rows = static_cast<std::int32_t>(resolution);

    // One fixed worker set for the whole simulation (spawning threads per
    // pass would cost more than the passes on small tiles); a barrier ends
    // each pass, like the GPU's barrier between dispatches.
    const auto workers = static_cast<std::int32_t>(
        glm::clamp(std::thread::hardware_concurrency(), 1U, glm::min(resolution, 16U)));
    std::barrier sync(workers);
    const auto run = [&](std::int32_t w) {
        const auto pass = [&](const auto& row) {
            for (std::int32_t z = w; z < rows; z += workers) row(z);
            sync.arrive_and_wait();
        };
        std::uint32_t cur = 0;
        for (std::uint32_t it = 0; it < params.iterations; ++it) {
            pass([&](std::int32_t z) { sim.flux_row(z); });
            pass([&](std::int32_t z) { sim.water_row(z); });
            pass([&](std::int32_t z) { sim.apply_row(z, cur); });
            pass([&](std::int32_t z) { sim.transport_row(z, cur); });
            cur ^= 1U;
        }
        for (std::int32_t z = w; z < rows; z += workers) sim.settle_row(z, cur);
    };
    // std::thread + explicit join (libc++ 18 gates std::jthread).
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers - 1));
    for (std::int32_t w = 1; w < workers; ++w) threads.emplace_back(run, w);
    run(0);
    for (std::thread& t : threads) t.join();
}

} // namespace engine::terrain
