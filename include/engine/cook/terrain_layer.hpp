#pragma once

// Terrain layer texture sets (ROADMAP R26.2): a PBR texture set (photo-
// scanned albedo, normal, roughness, ambient occlusion and displacement
// maps, e.g. a CC0 Poly Haven texture) cooked into the three KTX2 files the
// terrain material's texture arrays are built from:
//
//   albedo.ktx2  sRGB colour                       -> BC7 / ASTC 4x4 / RGBA8
//   normal.ktx2  tangent X / Y (+Y along +v, i.e.  -> BC5 / ASTC 4x4 (RA)
//                down the image; OpenGL-convention
//                sources are flipped)
//   packed.ktx2  R roughness, G occlusion,         -> BC7 / ASTC 4x4 / RGBA8
//                B height (normalised to 0..1)
//
// Every output is `size` x `size` (a power of two, so all sets of one cook
// share a mip chain and can be array slices together), with full mip chains
// in linear light (normals renormalised), Basis UASTC + Zstd.

#include <cstdint>
#include <expected>
#include <filesystem>

#include "engine/cook/gltf_cooker.hpp"
#include "engine/core/error.hpp"

namespace engine::cook {

struct TerrainLayerSources {
    std::filesystem::path albedo;    // required (sRGB)
    std::filesystem::path normal;    // required
    bool                  normal_opengl = true; // +Y up in the image (Poly Haven nor_gl)
    // Either an ARM map (R occlusion, G roughness, B metallic, as glTF / Poly
    // Haven pack it) or separate roughness / occlusion maps; missing ones
    // default to roughness 1, occlusion 1.
    std::filesystem::path arm;
    std::filesystem::path roughness;
    std::filesystem::path occlusion;
    std::filesystem::path height;    // displacement; missing: flat 0.5
};

struct TerrainLayerCookOptions {
    std::uint32_t size = 1024; // output side, power of two
    TextureCookOptions textures = [] {
        TextureCookOptions t;
        t.color_codec = BasisCodec::uastc; // close-up ground: BC7-grade colour
        t.normal_codec = BasisCodec::uastc;
        return t;
    }();
};

struct TerrainLayerCookStats {
    std::uint64_t input_bytes  = 0;
    std::uint64_t output_bytes = 0;
};

// Cooks the set into `out_dir` (created if needed): albedo.ktx2, normal.ktx2,
// packed.ktx2.
[[nodiscard]] std::expected<TerrainLayerCookStats, core::Error>
cook_terrain_layer(const TerrainLayerSources& sources, const std::filesystem::path& out_dir,
                   const TerrainLayerCookOptions& options = {});

} // namespace engine::cook
