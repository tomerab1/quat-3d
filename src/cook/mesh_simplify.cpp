// Cook-time level-of-detail reduction (quat-cook --simplify): one primitive's
// triangles reduced before the usual optimisation passes.
//
// The attribute-aware simplifier (normals and the first UV set in the error
// metric) keeps the mesh topology and UV seams, so it is right for solid
// models (rocks, logs, trunks). Foliage built from thousands of disconnected
// cards or needle clusters cannot get far that way: each component collapses
// to its own minimum. When the topology-preserving pass stops above twice
// the target, the primitive is thinned instead, the way foliage LODs are
// usually built: a deterministic subset of its components (leaf cards,
// needle clusters) is kept and each kept one grows about its centre so the
// foliage keeps its projected area, and with it its coverage from afar.
// (Vertex clustering would merge neighbouring cards into large triangles
// whose UVs span unrelated texels — alpha-tested, they render as shards.)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
// A kept card never grows by more than this (very low ratios would otherwise
// turn needles into planks).
constexpr float k_max_card_growth = 8.0F;

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

// A stable value in [0, 1) per component (independent of triangle order).
[[nodiscard]] float component_hash(std::uint32_t root) {
    std::uint64_t x = root + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return static_cast<float>(x >> 40U) * (1.0F / 16777216.0F);
}

// Keeps about `target` indices worth of whole connected components and grows
// the kept ones (see the file comment). Components with a vertex another
// primitive also uses keep their size.
[[nodiscard]] std::vector<std::uint32_t> thin_components(std::span<const std::uint32_t> indices,
                                                         Stream& positions,
                                                         std::span<const std::uint8_t> shared,
                                                         std::size_t target) {
    const std::size_t vertex_count = positions.values.size() / 3;
    std::vector<std::uint32_t> parent(vertex_count);
    std::iota(parent.begin(), parent.end(), 0U);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const std::uint32_t a = find_root(parent, indices[t]);
        parent[find_root(parent, indices[t + 1])] = a;
        parent[find_root(parent, indices[t + 2])] = a;
    }
    const float keep = static_cast<float>(target) / static_cast<float>(indices.size());
    std::vector<std::uint32_t> out;
    out.reserve(target + target / 4);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        if (component_hash(find_root(parent, indices[t])) < keep) {
            out.insert(out.end(), indices.begin() + static_cast<std::ptrdiff_t>(t),
                       indices.begin() + static_cast<std::ptrdiff_t>(t + 3));
        }
    }
    if (out.empty()) return out;

    // Grow each kept component about its centroid by 1 / sqrt(kept share).
    const float growth = std::min(
        k_max_card_growth,
        std::sqrt(static_cast<float>(indices.size()) / static_cast<float>(out.size())));
    std::vector<std::uint8_t> state(vertex_count, 0); // 1 = in a kept component
    for (const std::uint32_t v : out) state[v] = 1;
    struct Acc {
        double x = 0, y = 0, z = 0;
        std::uint32_t n = 0;
        bool fixed = false;
    };
    std::vector<Acc> acc(vertex_count); // indexed by root
    float* p = positions.values.data();
    for (std::uint32_t v = 0; v < vertex_count; ++v) {
        if (state[v] == 0) continue;
        Acc& a = acc[find_root(parent, v)];
        a.x += p[v * 3];
        a.y += p[v * 3 + 1];
        a.z += p[v * 3 + 2];
        ++a.n;
        a.fixed = a.fixed || (v < shared.size() && shared[v] != 0);
    }
    for (std::uint32_t v = 0; v < vertex_count; ++v) {
        if (state[v] == 0) continue;
        const Acc& a = acc[find_root(parent, v)];
        if (a.fixed) continue;
        const double inv = 1.0 / a.n;
        const double c[3] = {a.x * inv, a.y * inv, a.z * inv};
        for (int k = 0; k < 3; ++k) {
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
    out.resize(count);
    if (count > target * 2) {
        // Disconnected foliage: thin whole components from the original
        // triangles instead.
        out = thin_components(indices, positions, shared, target);
    }
    if (out.empty()) return indices.size(); // never erase a primitive outright
    indices = std::move(out);
    return indices.size();
}

} // namespace engine::cook::detail
