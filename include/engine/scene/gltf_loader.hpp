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

#include "engine/asset/mesh_asset.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::scene {

// CPU-side extracted geometry, before GPU upload. Exposed separately so the
// fastgltf extraction can be exercised without a device.
struct MeshData {
    std::vector<asset::Vertex>  vertices;
    std::vector<std::uint32_t>  indices;
    asset::Aabb                 bounds{};
    std::vector<asset::SubMesh> submeshes;
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

} // namespace engine::scene
