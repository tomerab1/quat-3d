// Talus relaxation (issue #69): the CPU reference of terrain_synth.slang's
// relax_* kernels. Each op below is the per-texel body of one dispatch, with
// the same operations in the same order; keep the two in sync (the header
// explains why they agree bit for bit).

#include "engine/terrain/talus_relax.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "parallel_rows.hpp"

namespace engine::terrain {

namespace {

// Interior sweep rate (talus_relax.hpp); x2 on an edge, x4 at a corner.
constexpr float k_rate = 0.0625F;
// Grids at least this wide split their rows over the hardware threads.
constexpr std::uint32_t k_parallel_resolution = 128;

[[nodiscard]] std::size_t at(std::uint32_t res, std::uint32_t x, std::uint32_t z) {
    return static_cast<std::size_t>(z) * res + x;
}

// 1 when index i is on the grid's border, else 0.
[[nodiscard]] std::uint32_t border(std::uint32_t i, std::uint32_t res) {
    return (i == 0 || i == res - 1) ? 1U : 0U;
}

// Full-weighting row sum around fine texel (fx, fz): h[-1] + 2 h[0] + h[+1]
// inside; h[0] + h[1] on the border, where a sample weighs half (the
// restriction is the volume-weighted transpose of the prolongation, so a
// coarse level holds the same volume as the fine one). Additions and
// doublings only; the normalisation (1/16, 1/8 on an edge, 1/4 at a corner)
// is a power of two.
[[nodiscard]] float weighted_row(const float* src, std::uint32_t res, std::uint32_t fx,
                                 std::uint32_t fz) {
    const float c = src[at(res, fx, fz)];
    if (fx == 0) return c + src[at(res, fx + 1, fz)];
    if (fx == res - 1) return src[at(res, fx - 1, fz)] + c;
    return src[at(res, fx - 1, fz)] + 2.0F * c + src[at(res, fx + 1, fz)];
}

void restrict_row(std::vector<float>& p, const TalusOp& op, std::uint32_t z) {
    const std::uint32_t fres = op.src_resolution;
    const float* src = p.data() + op.src;
    const std::uint32_t fz = 2 * z; // odd resolutions nest exactly: coarse i = fine 2i
    for (std::uint32_t x = 0; x < op.resolution; ++x) {
        const std::uint32_t fx = 2 * x;
        const float mid = weighted_row(src, fres, fx, fz);
        float sum = 0.0F;
        if (fz == 0) {
            sum = mid + weighted_row(src, fres, fx, fz + 1);
        } else if (fz == fres - 1) {
            sum = weighted_row(src, fres, fx, fz - 1) + mid;
        } else {
            sum = weighted_row(src, fres, fx, fz - 1) + 2.0F * mid +
                  weighted_row(src, fres, fx, fz + 1);
        }
        const std::uint32_t edges = border(fx, fres) + border(fz, fres);
        const float inv = edges == 0 ? 0.0625F : edges == 1 ? 0.125F : 0.25F;
        const float v = sum * inv;
        p[op.dst + at(op.resolution, x, z)] = v;
        p[op.orig + at(op.resolution, x, z)] = v;
    }
}

void sweep_row(std::vector<float>& p, const TalusOp& op, std::uint32_t z) {
    const auto res = static_cast<std::int32_t>(op.resolution);
    const auto zi = static_cast<std::int32_t>(z);
    const float* src = p.data() + op.src;
    const std::uint32_t z_edge = border(z, op.resolution);
    for (std::int32_t x = 0; x < res; ++x) {
        const float h = src[at(op.resolution, static_cast<std::uint32_t>(x), z)];
        float moved = 0.0F;
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const std::int32_t nx = x + dx;
                const std::int32_t nz = zi + dz;
                if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 || nx >= res || nz >= res) continue;
                const float limit = (dx != 0 && dz != 0) ? op.limit_diagonal : op.limit_axis;
                const float dh = src[at(op.resolution, static_cast<std::uint32_t>(nx),
                                        static_cast<std::uint32_t>(nz))] -
                                 h;
                const float excess = std::abs(dh) - limit;
                if (excess > 0.0F) moved += dh > 0.0F ? excess : -excess;
            }
        }
        // Border texels stand for half a cell (a quarter at a corner): the
        // same transfer changes their height twice (four times) as much.
        const std::uint32_t edges = border(static_cast<std::uint32_t>(x), op.resolution) + z_edge;
        const float rate = k_rate * static_cast<float>(1U << edges);
        p[op.dst + at(op.resolution, static_cast<std::uint32_t>(x), z)] = h + rate * moved;
    }
}

void prolong_row(std::vector<float>& p, const TalusOp& op, std::uint32_t z) {
    const std::uint32_t cres = op.src_resolution;
    const auto delta = [&](std::uint32_t x, std::uint32_t zz) {
        const std::size_t i = at(cres, x, zz);
        return p[op.src + i] - p[op.orig + i];
    };
    // Coarse texels around fine texel f: f even -> f/2, f odd -> f/2 and f/2 + 1.
    const std::uint32_t z0 = z / 2;
    const std::uint32_t z1 = z0 + (z & 1U);
    for (std::uint32_t x = 0; x < op.resolution; ++x) {
        const std::uint32_t x0 = x / 2;
        const std::uint32_t x1 = x0 + (x & 1U);
        const float d00 = delta(x0, z0);
        float v = d00;
        if (x0 != x1 && z0 != z1) {
            v = (d00 + delta(x1, z0) + delta(x0, z1) + delta(x1, z1)) * 0.25F;
        } else if (x0 != x1) {
            v = (d00 + delta(x1, z0)) * 0.5F;
        } else if (z0 != z1) {
            v = (d00 + delta(x0, z1)) * 0.5F;
        }
        float& out = p[op.dst + at(op.resolution, x, z)];
        out = out + v;
    }
}

} // namespace

TalusPyramid talus_pyramid(std::uint32_t resolution, float cell_m, float talus) {
    TalusPyramid pyr;
    const float slope = std::max(talus, 0.0F);
    const float axis = slope * cell_m;
    const float diagonal = slope * (cell_m * 1.41421356F);
    std::uint32_t res = resolution;
    for (int k = 0;; ++k) {
        TalusLevel level;
        level.resolution = res;
        const std::size_t n = static_cast<std::size_t>(res) * res;
        level.work = pyr.floats;
        level.swap = level.work + n;
        level.orig = k == 0 ? 0 : level.swap + n; // level 0 is never restricted into
        pyr.floats = level.swap + n + (k == 0 ? 0 : n);
        level.limit_axis = std::ldexp(axis, k); // x 2^k: exact
        level.limit_diagonal = std::ldexp(diagonal, k);
        pyr.levels.push_back(level);
        // Only odd grids nest exactly in the next coarser one (fine 2i =
        // coarse i, both borders kept).
        if (res <= talus_relax_coarsest || res % 2 == 0) break;
        res = res / 2 + 1;
    }
    return pyr;
}

std::vector<TalusOp> talus_schedule(const TalusPyramid& pyramid, std::uint32_t cycles) {
    std::vector<TalusOp> ops;
    const std::vector<TalusLevel>& lv = pyramid.levels;
    for (std::uint32_t c = 0; c < cycles; ++c) {
        for (std::size_t k = 1; k < lv.size(); ++k) {
            TalusOp op;
            op.kind = TalusOpKind::restrict_level;
            op.resolution = lv[k].resolution;
            op.src_resolution = lv[k - 1].resolution;
            op.src = lv[k - 1].work;
            op.dst = lv[k].work;
            op.orig = lv[k].orig;
            ops.push_back(op);
        }
        for (std::size_t k = lv.size(); k-- > 0;) {
            const std::uint32_t sweeps =
                k + 1 == lv.size() ? talus_relax_coarsest_sweeps : talus_relax_smoothing;
            for (std::uint32_t s = 0; s < sweeps; ++s) {
                TalusOp op;
                op.kind = TalusOpKind::sweep;
                op.resolution = lv[k].resolution;
                op.src_resolution = lv[k].resolution;
                op.src = s % 2 == 0 ? lv[k].work : lv[k].swap;
                op.dst = s % 2 == 0 ? lv[k].swap : lv[k].work;
                op.limit_axis = lv[k].limit_axis;
                op.limit_diagonal = lv[k].limit_diagonal;
                ops.push_back(op);
            }
            if (k == 0) continue;
            TalusOp op;
            op.kind = TalusOpKind::prolong;
            op.resolution = lv[k - 1].resolution;
            op.src_resolution = lv[k].resolution;
            op.src = lv[k].work;
            op.orig = lv[k].orig;
            op.dst = lv[k - 1].work;
            ops.push_back(op);
        }
    }
    return ops;
}

void run_talus_op(std::vector<float>& pyramid, const TalusOp& op) {
    const auto row = [&](std::uint32_t z) {
        switch (op.kind) {
        case TalusOpKind::restrict_level: restrict_row(pyramid, op, z); break;
        case TalusOpKind::sweep: sweep_row(pyramid, op, z); break;
        case TalusOpKind::prolong: prolong_row(pyramid, op, z); break;
        }
    };
    // Every op writes only its own rows and reads nothing another row of
    // the same op writes, so the split never changes the result.
    if (op.resolution >= k_parallel_resolution) {
        detail::parallel_rows(op.resolution, row);
    } else {
        for (std::uint32_t z = 0; z < op.resolution; ++z) row(z);
    }
}

bool relax_talus(std::vector<float>& heights_m, std::uint32_t resolution, float cell_m,
                 float talus, std::uint32_t cycles, const std::atomic<bool>* cancel) {
    if (cycles == 0 || resolution < 3 || !(cell_m > 0.0F) ||
        heights_m.size() != static_cast<std::size_t>(resolution) * resolution) {
        return true;
    }
    const TalusPyramid pyr = talus_pyramid(resolution, cell_m, talus);
    std::vector<float> buffer(pyr.floats, 0.0F);
    std::memcpy(buffer.data() + pyr.levels[0].work, heights_m.data(),
                heights_m.size() * sizeof(float));
    for (const TalusOp& op : talus_schedule(pyr, cycles)) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return false;
        run_talus_op(buffer, op);
    }
    std::memcpy(heights_m.data(), buffer.data() + pyr.levels[0].work,
                heights_m.size() * sizeof(float));
    return true;
}

} // namespace engine::terrain
