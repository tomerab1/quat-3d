#pragma once

// glTF parsing, validation and buffer access shared by the runtime loader
// (src/scene/gltf/) and the offline cooker (src/cook/). Only fastgltf types:
// no ECS, renderer or Vulkan dependencies (library engine_gltf_io).
//
//   validate.cpp        parse_gltf(), validate_asset()
//   meshopt_decode.cpp  EXT_meshopt_compression buffer views
//   gltf_source.cpp     data source bytes, texture image selection

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>

#include "engine/asset/texture_data.hpp"
#include "engine/core/error.hpp"

namespace engine::scene::gltf_detail {

[[nodiscard]] inline std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// No Options::GenerateMeshIndices: fastgltf runs it during parsing, before
// validate_asset() can range-check the accessor indices it dereferences.
// Non-indexed primitives get sequential indices in mesh.cpp instead.
inline constexpr auto kLoadOptions = fastgltf::Options::LoadExternalBuffers |
                                     fastgltf::Options::LoadExternalImages;

// glTF extensions the loader understands: material extensions, KTX2/Basis
// textures, and meshoptimizer-compressed / quantised geometry (quantised
// attributes are dequantised to the float vertex layout on extraction).
inline constexpr auto kParserExtensions = fastgltf::Extensions::KHR_materials_transmission |
                                          fastgltf::Extensions::KHR_materials_ior |
                                          fastgltf::Extensions::KHR_materials_volume |
                                          fastgltf::Extensions::KHR_texture_transform |
                                          fastgltf::Extensions::KHR_materials_emissive_strength |
                                          fastgltf::Extensions::KHR_materials_clearcoat |
                                          fastgltf::Extensions::KHR_texture_basisu |
                                          fastgltf::Extensions::EXT_meshopt_compression |
                                          fastgltf::Extensions::KHR_mesh_quantization;

// validate.cpp
[[nodiscard]] std::expected<void, core::Error> validate_asset(const fastgltf::Asset& asset);
// Parse with kParserExtensions / kLoadOptions, decode EXT_meshopt_compression
// views, then validate_asset(): the one entry every loader path goes through.
[[nodiscard]] std::expected<fastgltf::Asset, core::Error>
parse_gltf(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// meshopt_decode.cpp — replaces every compressed bufferView with a decoded
// buffer (fallback buffers are left unreferenced). The decoded bytes of all
// views together may not exceed `max_total_bytes`.
inline constexpr std::uint64_t k_max_meshopt_decoded_bytes = std::uint64_t{1} << 30;
[[nodiscard]] std::expected<void, core::Error>
decode_meshopt_buffers(fastgltf::Asset& asset,
                       std::uint64_t max_total_bytes = k_max_meshopt_decoded_bytes);

// gltf_source.cpp — the loaded bytes behind a buffer or image data source.
[[nodiscard]] std::expected<std::span<const std::byte>, core::Error>
data_source_bytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source);
// The glTF image a texture samples: its KHR_texture_basisu source when
// present (the core `source` is then only a fallback for other viewers).
[[nodiscard]] std::optional<std::size_t> texture_image(const fastgltf::Texture& texture);

// How materials sample each image: sRGB when used as base colour / emissive;
// role normal or single_channel only when that is the image's sole use.
// `fallback` marks the core `source` of a KHR_texture_basisu texture; such an
// image needs no decoding unless something else samples it.
// `alpha_test_cutoff` is the alphaCutoff of an alphaMode MASK material using
// the image as base colour (the lowest, when several do: it keeps the most
// coverage); its mips then preserve alpha-test coverage
// (engine/asset/alpha_coverage.hpp), at load and when cooked.
struct ImageUsage {
    asset::TextureRole       role        = asset::TextureRole::color;
    asset::TextureColorSpace color_space = asset::TextureColorSpace::linear;
    bool                     sampled     = false;
    bool                     fallback    = false;
    std::optional<float>     alpha_test_cutoff;
};
[[nodiscard]] std::vector<ImageUsage> image_usages(const fastgltf::Asset& asset);

} // namespace engine::scene::gltf_detail
