// Cook-time level-of-detail reduction (quat-cook --simplify): one primitive's
// triangles reduced before the usual optimisation passes.
//
// Solid primitives (rocks, logs, trunks) go through meshoptimizer's
// attribute-aware simplifier: quadric-error edge collapse (Garland &
// Heckbert, "Surface Simplification Using Quadric Error Metrics", SIGGRAPH
// 1997) with normals and the first UV set in the metric, within the error
// bound. If the bound stops it above the target, its result is kept: a LOD
// misses its triangle budget rather than change shape.
//
// Aggregate detail — foliage made of many small disconnected cards or needle
// clusters — must not go through edge collapse: clusters smaller than the
// error bound vanish whole, which reaches the triangle target by emptying
// the canopy (issue #99). Such a primitive is thinned instead, after Cook,
// Halstead, Planck & Ryu, "Stochastic Simplification of Aggregate Detail"
// (SIGGRAPH 2007): a deterministic pseudo-random subset of the elements
// (connected components) is kept, and each survivor is scaled about its
// centre by 1/sqrt(kept share), so the aggregate keeps its total area and
// with it its coverage from afar. The paper also adjusts the survivors'
// contrast / colour to keep the variance of the whole and prunes by
// distance at run time; these cook-time levels are its static form (the
// colour adjustment is not done here).
//
// Elements are found by position-welded connectivity, so a card whose
// vertices are split at UV seams or hard normals is still one element.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

#include <meshoptimizer.h>

#include "cook/internal.hpp"

namespace engine::cook::detail {

namespace {

// Weights of the attribute error relative to position error (meshoptimizer
// recommends values well below 1 for unit vectors / [0, 1] UVs).
constexpr float k_normal_weight = 0.5F;
constexpr float k_uv_weight = 0.5F;
// Aggregate detail: at least this many elements, averaging fewer triangles
// than k_element_triangles each. A rock set of a few disconnected boulders is
// solid; a canopy of thousands of needle clusters is aggregate.
constexpr std::size_t k_min_elements = 64;
constexpr double k_element_triangles = 24.0;
// A kept element never grows by more than this (very low ratios would
// otherwise turn needles into planks).
constexpr float k_max_element_growth = 8.0F;

[[nodiscard]] std::size_t target_index_count(std::size_t index_count, float ratio) {
    const auto target = static_cast<std::size_t>(static_cast<double>(index_count) * ratio);
    return std::max<std::size_t>(3, target / 3 * 3);
}

[[nodiscard]] std::uint32_t find_root(std::vector<std::uint32_t>& parent, std::uint32_t v) {
    std::uint32_t r = v;
    while (parent[r] != r) r = parent[r];
    while (parent[v] != r) {
        const std::uint32_t next = parent[v];
        parent[v] = r;
        v = next;
    }
    return r;
}

// Connected components of one primitive over position-welded vertices.
struct Elements {
    std::vector<std::uint32_t> element_of;   // per vertex: element id, or ~0 (unused)
    std::vector<std::uint32_t> lowest_vertex; // per element: smallest vertex index in it
    std::size_t                count = 0;
};

[[nodiscard]] Elements find_elements(std::span<const std::uint32_t> indices,
                                     const Stream& positions) {
    const std::size_t vertex_count = positions.values.size() / 3;
    std::vector<std::uint32_t> weld(vertex_count);
    meshopt_generatePositionRemap(weld.data(), positions.values.data(), vertex_count,
                                  sizeof(float) * 3);
    std::vector<std::uint32_t> parent(vertex_count);
    std::iota(parent.begin(), parent.end(), 0U);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const std::uint32_t a = find_root(parent, weld[indices[t]]);
        for (std::size_t k = 1; k < 3; ++k) {
            const std::uint32_t b = find_root(parent, weld[indices[t + k]]);
            if (a != b) parent[b] = a;
        }
    }
    Elements e;
    e.element_of.assign(vertex_count, ~0U);
    std::vector<std::uint32_t> id_of_root(vertex_count, ~0U);
    std::vector<std::uint8_t> used(vertex_count, 0);
    for (const std::uint32_t v : indices) used[v] = 1;
    for (std::uint32_t v = 0; v < vertex_count; ++v) { // ascending: lowest vertex first
        if (used[v] == 0) continue;
        const std::uint32_t root = find_root(parent, weld[v]);
        if (id_of_root[root] == ~0U) {
            id_of_root[root] = static_cast<std::uint32_t>(e.count++);
            e.lowest_vertex.push_back(v);
        }
        e.element_of[v] = id_of_root[root];
    }
    return e;
}

// A stable value in [0, 1) per element: a hash of its lowest vertex index,
// so the choice does not depend on triangle order.
[[nodiscard]] float element_hash(std::uint32_t lowest_vertex) {
    std::uint64_t x = lowest_vertex + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return static_cast<float>(x >> 40U) * (1.0F / 16777216.0F);
}

// Stochastic simplification of aggregate detail (see the file comment):
// keeps about `target` indices worth of whole elements and grows the kept
// ones. Elements with a vertex another primitive also uses keep their size.
[[nodiscard]] std::vector<std::uint32_t> thin_elements(std::span<const std::uint32_t> indices,
                                                       const Elements& e, Stream& positions,
                                                       std::span<const std::uint8_t> shared,
                                                       std::size_t target) {
    const float keep_share = static_cast<float>(target) / static_cast<float>(indices.size());
    std::vector<std::uint8_t> keep(e.count, 0);
    std::size_t first_choice = 0; // the element kept when the draw keeps none
    float lowest = 2.0F;
    for (std::size_t i = 0; i < e.count; ++i) {
        const float h = element_hash(e.lowest_vertex[i]);
        keep[i] = h < keep_share ? 1 : 0;
        if (h < lowest) {
            lowest = h;
            first_choice = i;
        }
    }
    if (std::none_of(keep.begin(), keep.end(), [](std::uint8_t k) { return k != 0; })) {
        keep[first_choice] = 1;
    }
    std::vector<std::uint32_t> out;
    out.reserve(target + target / 4);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        if (keep[e.element_of[indices[t]]] != 0) {
            out.insert(out.end(), indices.begin() + static_cast<std::ptrdiff_t>(t),
                       indices.begin() + static_cast<std::ptrdiff_t>(t + 3));
        }
    }

    // Grow each kept element about its centroid by 1 / sqrt(kept share).
    const float growth = std::min(
        k_max_element_growth,
        std::sqrt(static_cast<float>(indices.size()) / static_cast<float>(out.size())));
    struct Acc {
        double        x = 0.0, y = 0.0, z = 0.0;
        std::uint32_t n = 0;
        bool          fixed = false;
    };
    std::vector<Acc> acc(e.count);
    float* p = positions.values.data();
    const std::size_t vertex_count = positions.values.size() / 3;
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const std::uint32_t id = e.element_of[v];
        if (id == ~0U || keep[id] == 0) continue;
        Acc& a = acc[id];
        a.x += p[v * 3];
        a.y += p[v * 3 + 1];
        a.z += p[v * 3 + 2];
        ++a.n;
        a.fixed = a.fixed || (v < shared.size() && shared[v] != 0);
    }
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const std::uint32_t id = e.element_of[v];
        if (id == ~0U || keep[id] == 0 || acc[id].fixed) continue;
        const Acc& a = acc[id];
        const double inv = 1.0 / a.n;
        const double c[3] = {a.x * inv, a.y * inv, a.z * inv};
        for (std::size_t k = 0; k < 3; ++k) {
            p[v * 3 + k] = static_cast<float>(c[k] + (p[v * 3 + k] - c[k]) * growth);
        }
    }
    return out;
}

} // namespace

std::size_t simplify_triangles(std::vector<std::uint32_t>& indices, Stream& positions,
                               const Stream* normals, const Stream* uvs,
                               std::span<const std::uint8_t> shared, const MeshCookOptions& o) {
    const std::size_t vertex_count = positions.values.size() / 3;
    if (!(o.simplify_ratio > 0.0F && o.simplify_ratio < 1.0F) || indices.size() < 6) {
        return indices.size();
    }
    const std::size_t target = target_index_count(indices.size(), o.simplify_ratio);

    const Elements elements = find_elements(indices, positions);
    const bool aggregate =
        elements.count >= k_min_elements &&
        static_cast<double>(indices.size() / 3) / static_cast<double>(elements.count) <
            k_element_triangles;
    if (aggregate) {
        indices = thin_elements(indices, elements, positions, shared, target);
        return indices.size();
    }

    // The error bound is relative to the whole vertex set's extent (every
    // primitive sharing these streams), so each primitive of a model is
    // simplified to the same absolute tolerance.
    const float scale =
        meshopt_simplifyScale(positions.values.data(), vertex_count, sizeof(float) * 3);
    const float abs_error = o.simplify_error * scale;
    const unsigned options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;

    // Interleaved attribute metric: normal xyz, then uv.
    std::vector<float> attributes;
    std::vector<float> weights;
    std::size_t stride = 0;
    if (normals != nullptr && normals->components == 3) {
        stride += 3;
        weights.insert(weights.end(), 3, k_normal_weight);
    }
    if (uvs != nullptr && uvs->components == 2) {
        stride += 2;
        weights.insert(weights.end(), 2, k_uv_weight);
    }
    if (stride > 0) {
        attributes.resize(vertex_count * stride);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            float* dst = &attributes[v * stride];
            if (normals != nullptr && normals->components == 3) {
                std::copy_n(&normals->values[v * 3], 3, dst);
                dst += 3;
            }
            if (uvs != nullptr && uvs->components == 2) std::copy_n(&uvs->values[v * 2], 2, dst);
        }
    }

    std::vector<std::uint32_t> out(indices.size());
    std::size_t count = 0;
    if (stride > 0) {
        count = meshopt_simplifyWithAttributes(
            out.data(), indices.data(), indices.size(), positions.values.data(), vertex_count,
            sizeof(float) * 3, attributes.data(), stride * sizeof(float), weights.data(), stride,
            nullptr, target, abs_error, options, nullptr);
    } else {
        count = meshopt_simplify(out.data(), indices.data(), indices.size(),
                                 positions.values.data(), vertex_count, sizeof(float) * 3,
                                 target, abs_error, options, nullptr);
    }
    if (count == 0) return indices.size(); // never erase a primitive outright
    out.resize(count);
    indices = std::move(out);
    return indices.size();
}

} // namespace engine::cook::detail
