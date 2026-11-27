#pragma once

// KTX2 textures (ROADMAP R17): Basis Universal payloads (ETC1S / UASTC, as
// written by tools/quat-cook and used by glTF KHR_texture_basisu) transcoded
// at load to the best format the device samples, plus KTX2 files that
// already hold a GPU format. All bytes are untrusted: the header and level
// index are validated before anything is allocated or handed to libktx.
//
// Decoding is pure CPU work with no shared mutable state, so loaders run it
// on worker threads and only the upload touches the device.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>

#include "engine/asset/texture_asset.hpp"
#include "engine/asset/texture_limits.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/texture_caps.hpp"

namespace engine::asset {

// Payload of a validated KTX2 file.
enum class Ktx2Payload : std::uint8_t { etc1s, uastc, raw };

struct Ktx2Info {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    Ktx2Payload   payload = Ktx2Payload::raw;
    // Raw payloads: the stored format; nullopt for Basis payloads.
    std::optional<TexelFormat> raw_format;
    bool raw_srgb = false;
    // Bytes libktx holds once the levels are loaded (inflated for Zstd).
    std::uint64_t stored_bytes = 0;
};

// True when `bytes` start with the KTX2 file identifier.
[[nodiscard]] bool is_ktx2(std::span<const std::byte> bytes) noexcept;

// Structural validation of the header, data format descriptor range, level
// index and (ETC1S) BasisLZ global data against the file size and `limits`
// (no libktx involved). Rejects cubemaps, arrays, 3D and 1D textures,
// unsupported formats and supercompression schemes, level sizes that do not
// match the extent, and slices outside their level.
[[nodiscard]] std::expected<Ktx2Info, core::Error>
validate_ktx2(std::span<const std::byte> bytes, const TextureLimits& limits = {});

// Transcode target for a Basis payload: BC7 / BC5 (normal maps stored as X in
// RGB + Y in alpha) / BC4 (linear single channel) when BC is supported, ASTC
// 4x4 when only ASTC is (or the GPU prefers it), RGBA8 otherwise.
[[nodiscard]] TexelFormat select_transcode_format(const rhi::TextureCaps& caps,
                                                  TextureRole role,
                                                  TextureColorSpace color_space,
                                                  bool xy_normal_layout) noexcept;

struct Ktx2DecodeOptions {
    rhi::TextureCaps caps{};
    // How the texture is used (glTF material slot). When unset the role and
    // colour space come from the file's data format descriptor.
    std::optional<TextureRole>       role;
    std::optional<TextureColorSpace> color_space;
    // Applied after clamp_to_device(limits, caps).
    TextureLimits                    limits{};
    // The load's running budget (transcoded + inflated bytes), if any.
    DecodeBudget*                    budget = nullptr;
};

// Validates, inflates and (for Basis payloads) transcodes a KTX2 file into
// upload-ready TextureData with every mip level the file holds. A raw payload
// in a format the device cannot sample is an error.
[[nodiscard]] std::expected<TextureData, core::Error>
decode_ktx2(std::span<const std::byte> bytes, const Ktx2DecodeOptions& options);

// Reads and decodes a .ktx2 file from disk (size-capped by the limits).
[[nodiscard]] std::expected<TextureData, core::Error>
load_ktx2_file(const std::filesystem::path& path, const Ktx2DecodeOptions& options);

// AssetManager loader for a loose .ktx2 texture: decode with the device's
// formats from `transfer`, then upload.
[[nodiscard]] std::expected<TextureAsset, core::Error>
load_ktx2_texture(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                  const rhi::TransferContext& transfer, Ktx2DecodeOptions options = {});

} // namespace engine::asset
