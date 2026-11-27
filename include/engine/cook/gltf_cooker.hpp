#pragma once

// Offline asset cooker (ROADMAP R17), used by tools/quat-cook. Never linked
// into the game.
//
// cook_gltf() rewrites a glTF/GLB for shipping:
//   * images -> KTX2 with Basis Universal (KHR_texture_basisu), codec per
//     texture role, full mip chains, Zstd on UASTC;
//   * triangle meshes -> meshoptimizer vertex-cache / overdraw / vertex-fetch
//     optimisation,
//     quantised attributes (KHR_mesh_quantization) and the meshopt codecs
//     (EXT_meshopt_compression);
//   * buffers compacted to what the output still references.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

#include "engine/asset/texture_data.hpp"
#include "engine/core/error.hpp"

namespace engine::cook {

enum class BasisCodec : std::uint8_t { etc1s, uastc };

struct TextureCookOptions {
    bool       enabled      = true;
    BasisCodec color_codec  = BasisCodec::etc1s; // colour, ORM, occlusion, ...
    BasisCodec normal_codec = BasisCodec::uastc;
    // Store normal maps as X in RGB + Y in alpha (transcodes to BC5). Off
    // keeps plain RGB normals that other glTF viewers read directly.
    bool          normal_xy_layout = true;
    std::uint32_t etc1s_quality    = 128; // 1..255
    std::uint32_t uastc_level      = 2;   // 0 (fastest) .. 4 (slowest)
    // UASTC rate-distortion optimisation (makes Zstd far more effective);
    // lower is higher quality, 0 = off.
    float         uastc_rdo        = 1.0F;
    std::uint32_t zstd_level       = 18;  // UASTC supercompression; 0 = off
    bool          mipmaps          = true;
    std::uint32_t threads          = 0;   // encoder threads; 0 = all cores
};

struct MeshCookOptions {
    bool  optimize           = true;  // vertex cache + overdraw + vertex fetch
    float overdraw_threshold = 1.05F;
    bool  quantize           = true;  // KHR_mesh_quantization
    int   position_bits      = 16;    // exponent-filter mantissa bits (1..24)
    int   normal_bits        = 8;     // octahedral bits: 8 or 16 (also tangents)
    bool  compress           = true;  // EXT_meshopt_compression
};

struct CookOptions {
    TextureCookOptions textures;
    MeshCookOptions    meshes;
};

struct CookStats {
    std::uint64_t input_bytes        = 0; // .gltf/.glb plus external files
    std::uint64_t output_bytes       = 0;
    std::uint64_t image_bytes_before = 0;
    std::uint64_t image_bytes_after  = 0;
    std::uint64_t mesh_bytes_before  = 0; // vertex + index bytes of rewritten primitives
    std::uint64_t mesh_bytes_after   = 0;
    std::uint32_t images_encoded     = 0;
    std::uint32_t primitives_cooked  = 0;
    std::uint32_t primitives_skipped = 0; // morph targets, shared streams, non-triangles
};

// Cooks `input` into `output` (.glb writes a binary glTF, anything else a
// .gltf with a .bin next to it).
[[nodiscard]] std::expected<CookStats, core::Error>
cook_gltf(const std::filesystem::path& input, const std::filesystem::path& output,
          const CookOptions& options = {});

// One RGBA8 image (`width` x `height`, tightly packed) -> KTX2 bytes, as the
// glTF cook encodes a texture of `role` / `color_space`.
[[nodiscard]] std::expected<std::vector<std::byte>, core::Error>
encode_ktx2(std::span<const std::byte> rgba, std::uint32_t width, std::uint32_t height,
            asset::TextureRole role, asset::TextureColorSpace color_space,
            const TextureCookOptions& options = {});

} // namespace engine::cook
