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
// Every parse goes through parse_gltf() (gltf_io.hpp: fastgltf parse, meshopt
// decode, validate_asset()) before any extraction (glTF files are untrusted
// input).

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

#include "clip_compression.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "scene/gltf/gltf_io.hpp"

namespace engine::asset {
class AssetManager;
}

namespace engine::scene {
class Scene;
}

namespace engine::scene::gltf_detail {

[[nodiscard]] inline glm::vec3 to_glm(fastgltf::math::fvec3 v) { return {v[0], v[1], v[2]}; }
[[nodiscard]] inline glm::vec2 to_glm(fastgltf::math::fvec2 v) { return {v[0], v[1]}; }
[[nodiscard]] inline glm::vec4 to_glm(fastgltf::math::fvec4 v) {
    return {v[0], v[1], v[2], v[3]};
}

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

// texture.cpp — decodes every image a material samples (in parallel, all
// together within limits.max_total_decoded_bytes), with KTX2 images
// transcoded for `caps`. Images only referenced as a KHR_texture_basisu
// fallback are left empty.
[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset, const rhi::TextureCaps& caps = {},
                 const asset::TextureLimits& limits = {});

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
