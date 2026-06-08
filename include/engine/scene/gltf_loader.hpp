#pragma once

// glTF 2.0 static-mesh loading via fastgltf (Phase 3, Slice 3.2).
//
// Parses a .gltf/.glb file, merging every triangle primitive of every mesh into
// a single interleaved vertex buffer + uint32 index buffer, then uploads to
// device-local memory as a MeshAsset. Scene-graph nodes, skinning, and
// materials are deferred to later slices; this slice handles geometry only.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

#include <entt/entity/fwd.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {
class AssetManager;
}

namespace engine::scene {

class Scene;

// Sentinel for a material texture slot that the glTF material does not bind.
inline constexpr std::size_t no_texture = static_cast<std::size_t>(-1);

// CPU-side extracted geometry, before GPU upload. Exposed separately so the
// fastgltf extraction can be exercised without a device.
struct MeshData {
    std::vector<asset::Vertex>     vertices;
    std::vector<std::uint32_t>     indices;
    asset::Aabb                    bounds{};
    std::vector<asset::SubMesh>    submeshes;
    // Per-vertex skin influences, parallel to `vertices`, present only when the
    // glTF primitive(s) carry JOINTS_0/WEIGHTS_0. Empty for static meshes.
    std::vector<asset::SkinVertex> skin;
};

// CPU-side decoded texture (always RGBA8, tightly packed), before GPU upload.
// `color_space` is resolved from how the source image is referenced in glTF
// materials (base colour / emissive are sRGB, everything else is linear data).
struct TextureData {
    std::vector<std::byte>   pixels;
    std::uint32_t            width  = 0;
    std::uint32_t            height = 0;
    asset::TextureColorSpace color_space = asset::TextureColorSpace::linear;
};

// CPU-side material extracted from glTF: scalar PBR parameters (factors + flags)
// plus the glTF image index backing each texture slot (`no_texture` if absent).
// Texture *handles* are wired during scene assembly (later phase); this is the
// pure data the GBuffer pass and material upload need.
struct MaterialData {
    asset::PbrMaterialParams params;
    std::size_t base_color_image         = no_texture;
    std::size_t normal_image             = no_texture;
    std::size_t metallic_roughness_image = no_texture;
    std::size_t emissive_image           = no_texture;
    std::size_t occlusion_image          = no_texture;
};

class GltfLoader {
public:
    // Parse a glTF/GLB file into merged CPU geometry. External buffers/images
    // are resolved relative to the file's parent directory.
    [[nodiscard]] static std::expected<MeshData, core::Error>
    load_mesh_data(const std::filesystem::path& path);

    // Same, from an in-memory glTF/GLB image. `base_dir` resolves external URIs.
    [[nodiscard]] static std::expected<MeshData, core::Error>
    load_mesh_data_from_memory(std::span<const std::byte> bytes,
                               const std::filesystem::path& base_dir);

    // Full load: parse the file, then upload to device-local buffers.
    [[nodiscard]] static std::expected<asset::MeshAsset, core::Error>
    load(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
         const rhi::TransferContext& transfer);

    // Decode every glTF image into RGBA8 CPU pixels, tagging each with the colour
    // space implied by its material usage. No device needed.
    [[nodiscard]] static std::expected<std::vector<TextureData>, core::Error>
    load_texture_data(const std::filesystem::path& path);

    [[nodiscard]] static std::expected<std::vector<TextureData>, core::Error>
    load_texture_data_from_memory(std::span<const std::byte> bytes,
                                  const std::filesystem::path& base_dir);

    // Full texture load: decode all glTF images, then upload each to a sampleable
    // TextureAsset (GpuImage + view + sampler).
    [[nodiscard]] static std::expected<std::vector<asset::TextureAsset>, core::Error>
    load_textures(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                  const rhi::TransferContext& transfer);

    // Extract every glTF material's scalar parameters + texture image indices.
    // No device needed.
    [[nodiscard]] static std::expected<std::vector<MaterialData>, core::Error>
    load_material_data(const std::filesystem::path& path);

    [[nodiscard]] static std::expected<std::vector<MaterialData>, core::Error>
    load_material_data_from_memory(std::span<const std::byte> bytes,
                                   const std::filesystem::path& base_dir);

    // Extract every glTF skin as a SkeletonAsset (joint names, parent indices,
    // bind-pose TRS, inverse bind matrices). No device needed.
    [[nodiscard]] static std::expected<std::vector<animation::SkeletonAsset>, core::Error>
    load_skeletons(const std::filesystem::path& path);

    [[nodiscard]] static std::expected<std::vector<animation::SkeletonAsset>, core::Error>
    load_skeletons_from_memory(std::span<const std::byte> bytes,
                               const std::filesystem::path& base_dir);

    // Extract every glTF animation as an AnimClipAsset (samplers + channels).
    // No device needed.
    [[nodiscard]] static std::expected<std::vector<animation::AnimClipAsset>, core::Error>
    load_animations(const std::filesystem::path& path);

    [[nodiscard]] static std::expected<std::vector<animation::AnimClipAsset>, core::Error>
    load_animations_from_memory(std::span<const std::byte> bytes,
                                const std::filesystem::path& base_dir);

    // Instantiate a whole glTF scene into `scene` as ECS entities. Every mesh
    // primitive, image, and material is uploaded once and cached in `assets`
    // (keyed by `path` + index) so the MeshRenderer handles keep them alive.
    // Node TRS -> Transform.local; the glTF node hierarchy -> Parent/Children. A
    // node's mesh becomes a MeshRenderer on that entity (single-primitive mesh)
    // or one child entity per primitive (multi-material mesh), matching the
    // renderer's one-material-per-draw model. Returns the root entities created.
    [[nodiscard]] static std::expected<std::vector<entt::entity>, core::Error>
    instantiate(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                const rhi::TransferContext& transfer, asset::AssetManager& assets, Scene& scene);
};

// Decode a single encoded image (PNG/JPEG/etc.) into RGBA8 pixels via stb_image.
[[nodiscard]] std::expected<TextureData, core::Error>
decode_image(std::span<const std::byte> encoded,
             asset::TextureColorSpace color_space = asset::TextureColorSpace::linear);

// Upload decoded pixels to a device-local image and build its view + sampler.
[[nodiscard]] std::expected<asset::TextureAsset, core::Error>
upload_texture(const TextureData& data, rhi::GpuAllocator& allocator,
               const rhi::TransferContext& transfer);

// Upload already-extracted geometry into device-local vertex/index buffers.
[[nodiscard]] std::expected<asset::MeshAsset, core::Error>
upload_mesh(const MeshData& data, rhi::GpuAllocator& allocator,
            const rhi::TransferContext& transfer);

// Loads a tiny embedded triangle glTF from memory, checks the extracted counts
// and bounds, then uploads it — proving the parse + upload path end to end.
[[nodiscard]] std::expected<void, core::Error>
run_gltf_loader_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

// Decodes an embedded 2x2 PNG, verifies its pixels and colour-space tagging via
// a data-URI baseColorTexture, then uploads it — proving the texture path end to
// end.
[[nodiscard]] std::expected<void, core::Error>
run_texture_loader_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

// Parses an in-memory glTF material with known factors/flags and verifies the
// extracted MaterialData. No device needed.
[[nodiscard]] std::expected<void, core::Error>
run_material_extract_self_test();

// Instantiates a small two-node glTF hierarchy (a parent translated in X with a
// translated child, each carrying a mesh) and verifies the produced ECS entities:
// node count, Parent/Children links, local transforms, and that the child's world
// transform composes through its parent after a scene tick.
[[nodiscard]] std::expected<void, core::Error>
run_gltf_scene_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

// Parses an in-memory glTF skin (two-joint chain with inverse bind matrices) and
// verifies the extracted SkeletonAsset: joint count, names, parent indices,
// bind-pose TRS, and inverse-bind matrices. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_skeleton_load_self_test();

// Parses an in-memory glTF animation (translation LINEAR, scale STEP, rotation
// slerp on one node) and verifies the extracted clip's channels/samplers and a
// few sampled values. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_animation_load_self_test();

} // namespace engine::scene
