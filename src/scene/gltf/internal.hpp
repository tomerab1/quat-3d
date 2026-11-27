#pragma once

// Private to the glTF loader (src/scene/gltf/*.cpp): helpers shared between
// its translation units. Not part of the engine's public API.
//
//   validate.cpp        structural validation of a parsed asset
//   mesh.cpp            vertex/index extraction + mesh upload
//   texture.cpp         image decode (PNG/JPEG via stb, KTX2) + texture upload
//   material.cpp        PBR materials
//   meshopt_decode.cpp  EXT_meshopt_compression buffer views
//   skin_animation.cpp  skeletons and animation clips
//   scene_assembly.cpp  scene graph -> ECS entities
//
// Every parse goes through parse_gltf() (fastgltf parse, meshopt decode,
// validate_asset()) before any extraction (glTF files are untrusted input).

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/types.hpp>
#include <glm/glm.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/core/error.hpp"
#include "engine/scene/gltf_loader.hpp"

namespace engine::asset {
class AssetManager;
}

namespace engine::scene {
class Scene;
}

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

[[nodiscard]] inline glm::vec3 to_glm(fastgltf::math::fvec3 v) { return {v[0], v[1], v[2]}; }
[[nodiscard]] inline glm::vec2 to_glm(fastgltf::math::fvec2 v) { return {v[0], v[1]}; }
[[nodiscard]] inline glm::vec4 to_glm(fastgltf::math::fvec4 v) {
    return {v[0], v[1], v[2], v[3]};
}

// validate.cpp
[[nodiscard]] std::expected<void, core::Error> validate_asset(const fastgltf::Asset& asset);
// Parse with kParserExtensions / kLoadOptions, decode EXT_meshopt_compression
// views, then validate_asset(): the one entry every loader path goes through.
[[nodiscard]] std::expected<fastgltf::Asset, core::Error>
parse_gltf(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// meshopt_decode.cpp — replaces every compressed bufferView with a decoded
// buffer (fallback buffers are left unreferenced).
[[nodiscard]] std::expected<void, core::Error> decode_meshopt_buffers(fastgltf::Asset& asset);

// texture.cpp — the loaded bytes behind a buffer or image data source.
[[nodiscard]] std::expected<std::span<const std::byte>, core::Error>
data_source_bytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source);
// The glTF image a texture samples: its KHR_texture_basisu source when
// present (the core `source` is then only a fallback for other viewers).
[[nodiscard]] std::optional<std::size_t> texture_image(const fastgltf::Texture& texture);

// mesh.cpp
[[nodiscard]] std::expected<MeshData, core::Error> extract(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<MeshData, core::Error>
extract_primitive(const fastgltf::Asset& asset, const fastgltf::Primitive& prim);
[[nodiscard]] std::expected<MeshData, core::Error>
parse_and_extract(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);
// The largest JOINTS_0 index any vertex of `prim` uses with a non-zero
// WEIGHTS_0 weight; nullopt when the primitive is not skinned (or no vertex
// carries weight). Zero-weight influences are ignored: they contribute
// nothing, and the skinning shader clamps their indices.
[[nodiscard]] std::optional<std::uint32_t> max_weighted_joint(const fastgltf::Asset& asset,
                                                              const fastgltf::Primitive& prim);

// skin_animation.cpp
[[nodiscard]] glm::mat4 node_local_matrix(const fastgltf::Node& node);
[[nodiscard]] glm::mat4 to_glm_mat4(const fastgltf::math::fmat4x4& m);
[[nodiscard]] std::vector<animation::SkeletonAsset> extract_skeletons(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<animation::SkeletonAsset>, core::Error>
parse_and_extract_skeletons(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);
[[nodiscard]] std::vector<animation::AnimClipAsset> extract_animations(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<animation::AnimClipAsset>, core::Error>
parse_and_extract_animations(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// texture.cpp — decodes every image a material samples (in parallel), with
// KTX2 images transcoded for `caps`. Images only referenced as a
// KHR_texture_basisu fallback are left empty.
[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset, const rhi::TextureCompressionSupport& caps = {});

// material.cpp
[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
parse_and_extract_textures(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);
[[nodiscard]] std::vector<MaterialData> extract_materials(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<MaterialData>, core::Error>
parse_and_extract_materials(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// scene_assembly.cpp — every skinned node's mesh may only weight joints its
// skin has: JOINTS_0 indexes the skin's joint matrices on the GPU, so an index
// past the skin's joint count is an out-of-bounds read in the skinning pass.
// Only known per node (a mesh is skinned by whichever node instances it).
[[nodiscard]] std::expected<void, core::Error> validate_skin_bindings(const fastgltf::Asset& a);

// scene_assembly.cpp — build ECS entities from a parsed asset. `key_prefix`
// namespaces the AssetManager cache keys (two files' meshes don't collide).
[[nodiscard]] std::expected<std::vector<entt::entity>, core::Error>
assemble_scene(const fastgltf::Asset& a, const std::string& key_prefix,
               rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
               asset::AssetManager& assets, Scene& scene);

} // namespace engine::scene::gltf_detail
