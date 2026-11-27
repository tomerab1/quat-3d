// Cook-time level-of-detail reduction (quat-cook --simplify): one primitive's
// triangles reduced with meshoptimizer before the usual optimisation passes.
//
// The attribute-aware simplifier (normals and the first UV set in the error
// metric) keeps the mesh topology and UV seams, so it is right for solid
// models (rocks, logs, trunks). Foliage built from thousands of disconnected
// cards or needle clusters cannot get far that way: each component collapses
// to its own minimum. When the topology-preserving pass stops above twice
// the target, the sloppy simplifier (vertex clustering, topology ignored)
// takes over from the original triangles, which merges neighbouring leaves
// into larger alpha-tested triangles — the usual far-LOD trade for foliage.

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] std::size_t target_index_count(std::size_t index_count, float ratio) {
    const auto target = static_cast<std::size_t>(static_cast<double>(index_count) * ratio);
    return std::max<std::size_t>(3, target / 3 * 3);
}

} // namespace

std::size_t simplify_triangles(std::vector<std::uint32_t>& indices, const Stream& positions,
                               const Stream* normals, const Stream* uvs,
                               std::size_t vertex_count, const MeshCookOptions& o) {
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
    if (count > target * 2) {
        // Disconnected foliage: cluster from the original triangles instead,
        // as far as the requested ratio (the error bound would stop it at
        // the first merge of two leaves).
        count = meshopt_simplifySloppy(out.data(), indices.data(), indices.size(),
                                       positions.values.data(), vertex_count, sizeof(float) * 3,
                                       nullptr, target, 1.0F, nullptr);
    }
    if (count == 0) return indices.size(); // never erase a primitive outright
    out.resize(count);
    indices = std::move(out);
    return count;
}

} // namespace engine::cook::detail
