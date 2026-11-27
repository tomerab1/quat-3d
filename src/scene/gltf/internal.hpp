#pragma once

// Private to the glTF loader (src/scene/gltf/*.cpp): helpers shared between
// its translation units. Not part of the engine's public API.
//
//   validate.cpp        structural validation of a parsed asset
//   mesh.cpp            vertex/index extraction + mesh upload
//   material.cpp        image decode / texture upload + PBR materials
//   skin_animation.cpp  skeletons and animation clips
//   scene_assembly.cpp  scene graph -> ECS entities
//
// Every parse goes through parser.loadGltf(..., kLoadOptions) followed by
// validate_asset() before any extraction (glTF files are untrusted input).

#include <expected>
#include <filesystem>
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

// glTF material extensions the loader understands.
inline constexpr auto kParserExtensions = fastgltf::Extensions::KHR_materials_transmission |
                                          fastgltf::Extensions::KHR_materials_ior |
                                          fastgltf::Extensions::KHR_materials_volume |
                                          fastgltf::Extensions::KHR_texture_transform |
                                          fastgltf::Extensions::KHR_materials_emissive_strength |
                                          fastgltf::Extensions::KHR_materials_clearcoat;

[[nodiscard]] inline glm::vec3 to_glm(fastgltf::math::fvec3 v) { return {v[0], v[1], v[2]}; }
[[nodiscard]] inline glm::vec2 to_glm(fastgltf::math::fvec2 v) { return {v[0], v[1]}; }
[[nodiscard]] inline glm::vec4 to_glm(fastgltf::math::fvec4 v) {
    return {v[0], v[1], v[2], v[3]};
}

// validate.cpp
[[nodiscard]] std::expected<void, core::Error> validate_asset(const fastgltf::Asset& asset);

// mesh.cpp
[[nodiscard]] std::expected<MeshData, core::Error> extract(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<MeshData, core::Error>
extract_primitive(const fastgltf::Asset& asset, const fastgltf::Primitive& prim);
[[nodiscard]] std::expected<MeshData, core::Error>
parse_and_extract(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// skin_animation.cpp
[[nodiscard]] glm::mat4 node_local_matrix(const fastgltf::Node& node);
[[nodiscard]] glm::mat4 to_glm_mat4(const fastgltf::math::fmat4x4& m);
[[nodiscard]] std::vector<animation::SkeletonAsset> extract_skeletons(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<animation::SkeletonAsset>, core::Error>
parse_and_extract_skeletons(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);
[[nodiscard]] std::vector<animation::AnimClipAsset> extract_animations(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<animation::AnimClipAsset>, core::Error>
parse_and_extract_animations(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// material.cpp
[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
parse_and_extract_textures(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);
[[nodiscard]] std::vector<MaterialData> extract_materials(const fastgltf::Asset& asset);
[[nodiscard]] std::expected<std::vector<MaterialData>, core::Error>
parse_and_extract_materials(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir);

// scene_assembly.cpp — build ECS entities from a parsed asset. `key_prefix`
// namespaces the AssetManager cache keys (two files' meshes don't collide).
[[nodiscard]] std::expected<std::vector<entt::entity>, core::Error>
assemble_scene(const fastgltf::Asset& a, const std::string& key_prefix,
               rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
               asset::AssetManager& assets, Scene& scene);

} // namespace engine::scene::gltf_detail
