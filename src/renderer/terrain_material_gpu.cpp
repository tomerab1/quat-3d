// Terrain material packing and the CPU reference of the terrain attributes
// (see engine/renderer/terrain_material_gpu.hpp).

#include "engine/renderer/terrain_material_gpu.hpp"

#include <algorithm>
#include <cmath>

namespace engine::renderer {

glm::vec4 terrain_set_fallback(std::string_view set) {
    // Linear albedo means of typical CC0 scans of each material.
    if (set == "grass") return {0.085F, 0.12F, 0.04F, 0.92F};
    if (set == "dirt") return {0.16F, 0.11F, 0.07F, 0.95F};
    if (set == "rock") return {0.20F, 0.19F, 0.17F, 0.85F};
    if (set == "cliff") return {0.24F, 0.22F, 0.20F, 0.82F};
    if (set == "forest_floor") return {0.10F, 0.075F, 0.045F, 0.95F};
    if (set == "sand") return {0.45F, 0.36F, 0.24F, 0.9F};
    if (set == "snow") return {0.80F, 0.83F, 0.87F, 0.6F};
    return {0.18F, 0.18F, 0.18F, 0.9F};
}

TerrainMaterialGpu pack_terrain_material(const terrain::TerrainMaterial& m,
                                         std::span<const int> layer_slices, float mean_mip) {
    TerrainMaterialGpu g;
    const std::size_t n = std::min(m.layers.size(), terrain::max_material_layers);
    g.global0 = {static_cast<float>(n), m.blend_depth, m.triplanar_slope_deg,
                 m.triplanar_sharpness};
    g.global1 = {m.macro_strength, 1.0F / std::max(m.macro_scale_m, 1e-3F), m.far_start_m,
                 1.0F / std::max(m.far_range_m, 1e-3F)};
    g.global2 = {m.stochastic_tiling ? 1.0F : 0.0F, 0.0F, mean_mip, 0.0F};
    for (std::size_t i = 0; i < n; ++i) {
        const terrain::MaterialLayer& l = m.layers[i];
        const terrain::LayerRule& r = l.rule;
        TerrainLayerGpu& o = g.layers[i];
        o.tint_uv = {l.tint, 1.0F / std::max(l.uv_scale_m, 1e-3F)};
        o.slope = {r.slope_deg.x, r.slope_deg.y, r.slope_softness_deg, r.noise};
        o.altitude = {r.altitude_m.x, r.altitude_m.y, r.altitude_softness_m,
                      r.altitude_ref == terrain::AltitudeRef::snowline ? 1.0F : 0.0F};
        float paint = -1.0F;
        switch (r.paint) {
            case terrain::PaintChannel::r: paint = 0.0F; break;
            case terrain::PaintChannel::g: paint = 1.0F; break;
            case terrain::PaintChannel::b: paint = 2.0F; break;
            case terrain::PaintChannel::none: break;
        }
        o.affinity = {r.concavity, r.flow, r.strength, paint};
        const int slice = i < layer_slices.size() ? layer_slices[i] : -1;
        o.surface = {l.normal_strength, l.roughness, l.height_offset, static_cast<float>(slice)};
        o.fallback = terrain_set_fallback(l.texture_set);
    }
    return g;
}

namespace {

constexpr int k_dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int k_dz[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

} // namespace

std::vector<glm::vec2> terrain_attributes_reference(std::span<const float> heights,
                                                    std::uint32_t resolution, float mpt,
                                                    std::uint32_t sweeps) {
    const auto r = static_cast<int>(resolution);
    const std::size_t n = static_cast<std::size_t>(resolution) * resolution;
    const auto at = [&](int x, int z) {
        x = std::clamp(x, 0, r - 1);
        z = std::clamp(z, 0, r - 1);
        return heights[static_cast<std::size_t>(z) * resolution + static_cast<std::size_t>(x)];
    };
    const auto inside = [&](int x, int z) { return x >= 0 && z >= 0 && x < r && z < r; };
    const auto weight = [&](float hf, float ht, int ox, int oz) {
        const bool diagonal = ox != 0 && oz != 0;
        const float dist = mpt * (diagonal ? 1.41421356F : 1.0F);
        const float drop = hf - ht;
        if (drop <= 0.0F) return 0.0F;
        return std::pow(drop / dist, 1.1F) * (diagonal ? 0.35355339F : 0.5F);
    };

    std::vector<float> wsum(n, 0.0F);
    for (int z = 0; z < r; ++z) {
        for (int x = 0; x < r; ++x) {
            float s = 0.0F;
            for (int k = 0; k < 8; ++k) {
                if (inside(x + k_dx[k], z + k_dz[k])) {
                    s += weight(at(x, z), at(x + k_dx[k], z + k_dz[k]), k_dx[k], k_dz[k]);
                }
            }
            wsum[static_cast<std::size_t>(z) * resolution + static_cast<std::size_t>(x)] = s;
        }
    }
    std::vector<float> flow(n, 1.0F);
    std::vector<float> next(n, 1.0F);
    for (std::uint32_t it = 0; it < sweeps; ++it) {
        for (int z = 0; z < r; ++z) {
            for (int x = 0; x < r; ++x) {
                float acc = 1.0F;
                for (int k = 0; k < 8; ++k) {
                    const int qx = x + k_dx[k];
                    const int qz = z + k_dz[k];
                    if (!inside(qx, qz)) continue;
                    const std::size_t qi = static_cast<std::size_t>(qz) * resolution +
                                           static_cast<std::size_t>(qx);
                    if (wsum[qi] <= 0.0F) continue;
                    acc += flow[qi] * (weight(at(qx, qz), at(x, z), -k_dx[k], -k_dz[k]) / wsum[qi]);
                }
                next[static_cast<std::size_t>(z) * resolution + static_cast<std::size_t>(x)] = acc;
            }
        }
        flow.swap(next);
    }

    const auto ring = [&](int x, int z, int d) {
        return 0.125F * (at(x + d, z) + at(x - d, z) + at(x, z + d) + at(x, z - d) +
                         at(x + d, z + d) + at(x - d, z - d) + at(x + d, z - d) +
                         at(x - d, z + d));
    };
    std::vector<glm::vec2> out(n);
    for (int z = 0; z < r; ++z) {
        for (int x = 0; x < r; ++x) {
            const float h = at(x, z);
            const float c2 = (ring(x, z, 2) - h) / (2.0F * mpt);
            const float c6 = (ring(x, z, 6) - h) / (6.0F * mpt);
            const float concavity = std::clamp(2.5F * (c2 + c6), -1.0F, 1.0F);
            const std::size_t i = static_cast<std::size_t>(z) * resolution + static_cast<std::size_t>(x);
            const float wet = std::clamp(std::log2(std::max(flow[i], 1.0F)) / 8.0F, 0.0F, 1.0F);
            out[i] = {concavity * 0.5F + 0.5F, wet};
        }
    }
    return out;
}

} // namespace engine::renderer
