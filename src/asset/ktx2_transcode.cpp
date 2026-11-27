// KTX2 decode: libktx inflates (Zstd) and transcodes (Basis Universal) a
// file that validate_ktx2() has already range-checked.

#include "engine/asset/ktx2_texture.hpp"

#include <KHR/khr_df.h>
#include <ktx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"KTX2: " + std::move(what)});
}

struct KtxDeleter {
    void operator()(ktxTexture2* t) const noexcept { ktxTexture2_Destroy(t); }
};
using KtxPtr = std::unique_ptr<ktxTexture2, KtxDeleter>;

// What the data format descriptor says about a Basis payload.
struct DfdTraits {
    bool srgb = false;
    bool single_channel = false;
    bool xy_normal = false; // X in RGB, Y in alpha (Basis normal-map layout)
};

[[nodiscard]] std::expected<DfdTraits, core::Error> read_dfd(const ktxTexture2& tex) {
    const ktx_uint32_t* dfd = tex.pDfd;
    if (dfd == nullptr) return fail("missing data format descriptor");
    const ktx_uint32_t total = dfd[0];
    const ktx_uint32_t* bdb = dfd + 1;
    const ktx_uint32_t block = KHR_DFDVAL(bdb, DESCRIPTORBLOCKSIZE);
    const ktx_uint32_t header_bytes = KHR_DF_WORD_SAMPLESTART * 4U;
    if (block < header_bytes || total < 4U || block > total - 4U) {
        return fail("malformed data format descriptor");
    }
    const ktx_uint32_t samples = (block - header_bytes) / (KHR_DF_WORD_SAMPLEWORDS * 4U);
    if (samples == 0) return fail("data format descriptor has no samples");

    DfdTraits t;
    t.srgb = KHR_DFDVAL(bdb, TRANSFER) == KHR_DF_TRANSFER_SRGB;
    const ktx_uint32_t first = KHR_DFDSVAL(bdb, 0, CHANNELID);
    if (KHR_DFDVAL(bdb, MODEL) == KHR_DF_MODEL_UASTC) {
        t.xy_normal = first == KHR_DF_CHANNEL_UASTC_RRRG;
        t.single_channel = first == KHR_DF_CHANNEL_UASTC_RRR;
    } else {
        const ktx_uint32_t second = samples > 1 ? KHR_DFDSVAL(bdb, 1, CHANNELID) : first;
        t.xy_normal = samples == 2 && first == KHR_DF_CHANNEL_ETC1S_RRR &&
                      second == KHR_DF_CHANNEL_ETC1S_GGG;
        t.single_channel = samples == 1 && first == KHR_DF_CHANNEL_ETC1S_RRR;
    }
    return t;
}

[[nodiscard]] ktx_transcode_fmt_e ktx_target(TexelFormat f) noexcept {
    switch (f) {
        case TexelFormat::bc7:      return KTX_TTF_BC7_RGBA;
        case TexelFormat::bc5:      return KTX_TTF_BC5_RG;
        case TexelFormat::bc4:      return KTX_TTF_BC4_R;
        case TexelFormat::astc_4x4: return KTX_TTF_ASTC_4x4_RGBA;
        case TexelFormat::rgba8:    return KTX_TTF_RGBA32;
    }
    return KTX_TTF_RGBA32;
}

// libktx initialises the Basis transcoder tables lazily behind an unguarded
// static flag. Until one transcode has completed, transcodes are serialised so
// that initialisation happens exactly once; afterwards they run concurrently.
std::mutex        g_first_transcode;
std::atomic<bool> g_transcoder_ready{false};

[[nodiscard]] ktx_error_code_e transcode(ktxTexture2* tex, ktx_transcode_fmt_e fmt) {
    if (g_transcoder_ready.load(std::memory_order_acquire)) {
        return ktxTexture2_TranscodeBasis(tex, fmt, 0);
    }
    const std::lock_guard<std::mutex> lock(g_first_transcode);
    const ktx_error_code_e r = ktxTexture2_TranscodeBasis(tex, fmt, 0);
    if (r == KTX_SUCCESS) g_transcoder_ready.store(true, std::memory_order_release);
    return r;
}

[[nodiscard]] bool device_samples(const rhi::TextureCompressionSupport& caps,
                                  TexelFormat f) noexcept {
    switch (f) {
        case TexelFormat::rgba8:    return true;
        case TexelFormat::astc_4x4: return caps.astc_ldr;
        case TexelFormat::bc7:
        case TexelFormat::bc5:
        case TexelFormat::bc4:      return caps.bc;
    }
    return false;
}

} // namespace

TexelFormat select_transcode_format(const rhi::TextureCompressionSupport& caps,
                                    TextureRole role, TextureColorSpace color_space,
                                    bool xy_normal_layout) noexcept {
    const bool astc = caps.astc_ldr && (caps.prefer_astc || !caps.bc);
    if (astc) return TexelFormat::astc_4x4;
    if (!caps.bc) return TexelFormat::rgba8;
    // BC5 / BC4 have no sRGB variant; the transcoder fills BC5 from R and A,
    // so only the X-in-RGB / Y-in-alpha normal layout maps onto it.
    if (role == TextureRole::normal && xy_normal_layout) return TexelFormat::bc5;
    if (role == TextureRole::single_channel && color_space == TextureColorSpace::linear) {
        return TexelFormat::bc4;
    }
    return TexelFormat::bc7;
}

std::expected<TextureData, core::Error> decode_ktx2(std::span<const std::byte> bytes,
                                                     const Ktx2DecodeOptions& options) {
    auto info = validate_ktx2(bytes, options.limits);
    if (!info) return std::unexpected(info.error());

    ktxTexture2* raw_tex = nullptr;
    const ktx_error_code_e created = ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(bytes.data()), bytes.size(),
        static_cast<ktxTextureCreateFlags>(KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT |
                                           KTX_TEXTURE_CREATE_SKIP_KVDATA_BIT),
        &raw_tex);
    KtxPtr tex(raw_tex);
    if (created != KTX_SUCCESS || !tex) {
        return fail(std::string("libktx rejected the file: ") + ktxErrorString(created));
    }

    TextureData out;
    out.width = info->width;
    out.height = info->height;

    if (info->payload == Ktx2Payload::raw) {
        out.format = *info->raw_format;
        if (!device_samples(options.caps, out.format)) {
            return fail("stored format is not supported by this device");
        }
        out.color_space = options.color_space.value_or(
            info->raw_srgb ? TextureColorSpace::srgb : TextureColorSpace::linear);
        if (out.format == TexelFormat::bc4) out.swizzle = TextureSwizzle::rrr1;
    } else {
        auto traits = read_dfd(*tex);
        if (!traits) return std::unexpected(traits.error());
        const TextureRole role = options.role.value_or(
            traits->xy_normal        ? TextureRole::normal
            : traits->single_channel ? TextureRole::single_channel
                                     : TextureRole::color);
        out.color_space = options.color_space.value_or(
            traits->srgb ? TextureColorSpace::srgb : TextureColorSpace::linear);
        out.format = select_transcode_format(options.caps, role, out.color_space,
                                             traits->xy_normal);
        std::uint64_t target_bytes = 0;
        for (std::uint32_t level = 0; level < info->levels; ++level) {
            target_bytes += texel_level_size(out.format, std::max(info->width >> level, 1U),
                                             std::max(info->height >> level, 1U));
        }
        if (target_bytes > options.limits.max_decoded_bytes) {
            return fail("transcoded size exceeds the configured limit");
        }
        if (const ktx_error_code_e r = transcode(tex.get(), ktx_target(out.format));
            r != KTX_SUCCESS) {
            return fail(std::string("Basis transcode failed: ") + ktxErrorString(r));
        }
        if (traits->xy_normal && out.format != TexelFormat::bc5) {
            out.swizzle = TextureSwizzle::ra01;
        } else if (out.format == TexelFormat::bc4) {
            out.swizzle = TextureSwizzle::rrr1;
        }
    }

    // Repack the levels contiguously, level 0 first, checking libktx's
    // offsets against its own data buffer.
    std::uint64_t total = 0;
    out.levels.resize(info->levels);
    for (std::uint32_t level = 0; level < info->levels; ++level) {
        TextureLevel& l = out.levels[level];
        l.width = std::max(info->width >> level, 1U);
        l.height = std::max(info->height >> level, 1U);
        l.offset = total;
        l.size = texel_level_size(out.format, l.width, l.height);
        total += l.size;
    }
    out.pixels.resize(total);
    for (std::uint32_t level = 0; level < info->levels; ++level) {
        ktx_size_t offset = 0;
        if (ktxTexture2_GetImageOffset(tex.get(), level, 0, 0, &offset) != KTX_SUCCESS ||
            offset > tex->dataSize || out.levels[level].size > tex->dataSize - offset ||
            ktxTexture_GetImageSize(ktxTexture(tex.get()), level) != out.levels[level].size) {
            return fail("decoded level " + std::to_string(level) + " has an unexpected size");
        }
        std::memcpy(out.pixels.data() + out.levels[level].offset, tex->pData + offset,
                    out.levels[level].size);
    }
    return out;
}

std::expected<TextureData, core::Error> load_ktx2_file(const std::filesystem::path& path,
                                                        const Ktx2DecodeOptions& options) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("cannot stat '" + path.string() + "': " + ec.message());
    if (size > options.limits.max_file_bytes) {
        return fail("'" + path.string() + "' is larger than the configured limit");
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.string().c_str(), "rb"),
                                                         &std::fclose);
    if (!file) return fail("cannot open '" + path.string() + "'");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
        return fail("short read on '" + path.string() + "'");
    }
    return decode_ktx2(bytes, options);
}

std::expected<TextureAsset, core::Error>
load_ktx2_texture(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                  const rhi::TransferContext& transfer, Ktx2DecodeOptions options) {
    options.caps = transfer.compression;
    auto data = load_ktx2_file(path, options);
    if (!data) return std::unexpected(data.error());
    return upload_texture_data(*data, allocator, transfer);
}

} // namespace engine::asset
