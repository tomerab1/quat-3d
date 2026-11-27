// glTF cook: parse -> meshes (mesh_cook.cpp) -> images (KTX2) -> one compacted
// binary buffer plus an EXT_meshopt_compression fallback buffer -> export.

#include <algorithm>
#include <cstring>
#include <string>
#include <variant>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <meshoptimizer.h>

// Declarations only; the implementation is compiled once, permissively.
#include <stb_image.h>

#include "cook/internal.hpp"

namespace engine::cook {

namespace {

using detail::fail;
namespace fs = std::filesystem;

constexpr auto k_extensions =
    fastgltf::Extensions::KHR_materials_transmission | fastgltf::Extensions::KHR_materials_ior |
    fastgltf::Extensions::KHR_materials_volume | fastgltf::Extensions::KHR_texture_transform |
    fastgltf::Extensions::KHR_materials_emissive_strength |
    fastgltf::Extensions::KHR_materials_clearcoat | fastgltf::Extensions::KHR_texture_basisu |
    fastgltf::Extensions::EXT_meshopt_compression | fastgltf::Extensions::KHR_mesh_quantization;

[[nodiscard]] bool uses_extension(const fastgltf::Asset& asset, std::string_view name) {
    return std::any_of(asset.extensionsUsed.begin(), asset.extensionsUsed.end(),
                       [&](const auto& e) { return std::string_view(e) == name; });
}

void require_extension(fastgltf::Asset& asset, std::string_view name) {
    if (!uses_extension(asset, name)) asset.extensionsUsed.emplace_back(name);
    if (std::none_of(asset.extensionsRequired.begin(), asset.extensionsRequired.end(),
                     [&](const auto& e) { return std::string_view(e) == name; })) {
        asset.extensionsRequired.emplace_back(name);
    }
}

struct ImageRoles {
    std::vector<asset::TextureRole>       role;
    std::vector<asset::TextureColorSpace> space;
    std::vector<bool>                     sampled;
};

// Same usage rules as the runtime loader (scene/gltf/texture.cpp).
[[nodiscard]] ImageRoles image_roles(const fastgltf::Asset& a) {
    const std::size_t n = a.images.size();
    std::vector<std::uint8_t> srgb(n), normal(n), single(n), other(n);
    ImageRoles r{std::vector<asset::TextureRole>(n, asset::TextureRole::color),
                 std::vector<asset::TextureColorSpace>(n, asset::TextureColorSpace::linear),
                 std::vector<bool>(n, false)};
    const auto mark = [&](const auto& info, std::vector<std::uint8_t>& flag) {
        if (!info.has_value() || info->textureIndex >= a.textures.size()) return;
        const auto& img = a.textures[info->textureIndex].imageIndex;
        if (img && *img < n) flag[*img] = 1;
    };
    for (const fastgltf::Material& m : a.materials) {
        mark(m.pbrData.baseColorTexture, srgb);
        mark(m.emissiveTexture, srgb);
        mark(m.normalTexture, normal);
        mark(m.occlusionTexture, single);
        mark(m.pbrData.metallicRoughnessTexture, other);
        if (m.volume != nullptr) mark(m.volume->thicknessTexture, other);
    }
    for (const fastgltf::Texture& t : a.textures) {
        if (t.imageIndex && *t.imageIndex < n && !t.basisuImageIndex) r.sampled[*t.imageIndex] = true;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (srgb[i] != 0) r.space[i] = asset::TextureColorSpace::srgb;
        if (srgb[i] == 0 && other[i] == 0) {
            if (normal[i] != 0 && single[i] == 0) r.role[i] = asset::TextureRole::normal;
            if (single[i] != 0 && normal[i] == 0) r.role[i] = asset::TextureRole::single_channel;
        }
    }
    return r;
}

// Encodes every sampled PNG/JPEG image to KTX2; nullopt keeps an image as is.
[[nodiscard]] std::expected<std::vector<std::optional<std::vector<std::byte>>>, core::Error>
cook_images(const fastgltf::Asset& asset, const TextureCookOptions& o, CookStats& stats) {
    std::vector<std::optional<std::vector<std::byte>>> out(asset.images.size());
    if (!o.enabled) return out;
    const ImageRoles roles = image_roles(asset);
    for (std::size_t i = 0; i < asset.images.size(); ++i) {
        if (!roles.sampled[i]) continue;
        auto encoded = detail::source_bytes(asset, asset.images[i].data);
        if (!encoded) return std::unexpected(encoded.error());
        int w = 0;
        int h = 0;
        int ch = 0;
        stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded->data()),
                                            static_cast<int>(encoded->size()), &w, &h, &ch, 4);
        if (px == nullptr) continue; // not PNG/JPEG (e.g. KTX2 already): keep
        const std::span<const std::byte> rgba(reinterpret_cast<const std::byte*>(px),
                                              static_cast<std::size_t>(w) * h * 4);
        auto ktx2 = detail::encode_image_ktx2(rgba, static_cast<std::uint32_t>(w),
                                              static_cast<std::uint32_t>(h), roles.role[i],
                                              roles.space[i], o);
        stbi_image_free(px);
        if (!ktx2) return fail("image " + std::to_string(i) + ": " + ktx2.error().message);
        stats.image_bytes_before += encoded->size();
        stats.image_bytes_after += ktx2->size();
        ++stats.images_encoded;
        out[i] = std::move(*ktx2);
    }
    return out;
}

void append_aligned(std::vector<std::byte>& buffer, std::span<const std::byte> bytes,
                    std::size_t& offset) {
    buffer.resize((buffer.size() + 3) / 4 * 4);
    offset = buffer.size();
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

// meshopt codec (version 0 vertex codec, as EXT_meshopt_compression requires).
[[nodiscard]] std::vector<std::byte> encode_view(const detail::PendingView& v) {
    const std::vector<std::byte>& src = v.filter != fastgltf::MeshoptCompressionFilter::None
                                            ? v.filtered
                                            : v.raw;
    std::vector<unsigned char> enc;
    std::size_t size = 0;
    if (v.mode == fastgltf::MeshoptCompressionMode::Attributes) {
        enc.resize(meshopt_encodeVertexBufferBound(v.count, v.stride));
        size = meshopt_encodeVertexBufferLevel(enc.data(), enc.size(), src.data(), v.count,
                                               v.stride, 2, 0);
    } else {
        std::vector<std::uint32_t> idx(v.count);
        for (std::size_t i = 0; i < v.count; ++i) {
            if (v.stride == 2) {
                std::uint16_t s = 0;
                std::memcpy(&s, src.data() + i * 2, 2);
                idx[i] = s;
            } else {
                std::memcpy(&idx[i], src.data() + i * 4, 4);
            }
        }
        const std::size_t vertices =
            idx.empty() ? 0 : static_cast<std::size_t>(*std::max_element(idx.begin(), idx.end())) + 1;
        enc.resize(meshopt_encodeIndexBufferBound(v.count, vertices));
        size = meshopt_encodeIndexBuffer(enc.data(), enc.size(), idx.data(), v.count);
    }
    enc.resize(size);
    const auto* b = reinterpret_cast<const std::byte*>(enc.data());
    return {b, b + enc.size()};
}

struct Assembly {
    std::vector<std::byte>          buffer;
    std::size_t                     fallback_bytes = 0;
    std::vector<fastgltf::BufferView> views;
};

// Rebuilds buffers and views: kept original views are copied, new views
// appended (meshopt-compressed or raw), encoded images embedded.
[[nodiscard]] std::expected<void, core::Error>
assemble(fastgltf::Asset& asset, detail::GeometryResult& geo,
         std::vector<std::optional<std::vector<std::byte>>>& images, const MeshCookOptions& o,
         CookStats& stats) {
    Assembly out;
    std::vector<std::optional<std::size_t>> old_to_new(asset.bufferViews.size());
    const auto keep_view = [&](std::size_t old) -> std::expected<std::size_t, core::Error> {
        if (old >= old_to_new.size()) return fail("bufferView index out of range");
        if (old_to_new[old]) return *old_to_new[old];
        auto bytes = detail::view_bytes(asset, old);
        if (!bytes) return std::unexpected(bytes.error());
        const fastgltf::BufferView& src = asset.bufferViews[old];
        fastgltf::BufferView v{};
        v.bufferIndex = 0;
        append_aligned(out.buffer, *bytes, v.byteOffset);
        v.byteLength = bytes->size();
        v.byteStride = src.byteStride;
        v.target = src.target;
        out.views.push_back(std::move(v));
        old_to_new[old] = out.views.size() - 1;
        return out.views.size() - 1;
    };

    std::vector<std::size_t> pending_to_new(geo.views.size());
    std::uint64_t mesh_after = 0;
    for (std::size_t p = 0; p < geo.views.size(); ++p) {
        const detail::PendingView& pv = geo.views[p];
        fastgltf::BufferView v{};
        v.byteLength = pv.raw.size();
        if (pv.vertex) v.byteStride = pv.stride;
        v.target = pv.vertex ? fastgltf::BufferTarget::ArrayBuffer
                             : fastgltf::BufferTarget::ElementArrayBuffer;
        if (o.compress) {
            const std::vector<std::byte> enc = encode_view(pv);
            auto c = std::make_unique<fastgltf::CompressedBufferView>();
            c->bufferIndex = 0;
            append_aligned(out.buffer, enc, c->byteOffset);
            c->byteLength = enc.size();
            c->byteStride = pv.stride;
            c->count = pv.count;
            c->mode = pv.mode;
            c->filter = pv.filter;
            v.bufferIndex = 1;
            out.fallback_bytes = (out.fallback_bytes + 3) / 4 * 4;
            v.byteOffset = out.fallback_bytes;
            out.fallback_bytes += pv.raw.size();
            v.meshoptCompression = std::move(c);
            mesh_after += enc.size();
        } else {
            v.bufferIndex = 0;
            append_aligned(out.buffer, pv.raw, v.byteOffset);
            mesh_after += pv.raw.size();
        }
        out.views.push_back(std::move(v));
        pending_to_new[p] = out.views.size() - 1;
    }

    for (std::size_t i = 0; i < asset.accessors.size(); ++i) {
        fastgltf::Accessor& a = asset.accessors[i];
        if (i < geo.accessor_view.size() && geo.accessor_view[i]) {
            a.bufferViewIndex = pending_to_new[*geo.accessor_view[i]];
            continue;
        }
        if (a.bufferViewIndex) {
            auto v = keep_view(*a.bufferViewIndex);
            if (!v) return std::unexpected(v.error());
            a.bufferViewIndex = *v;
        }
        if (a.sparse) {
            auto iv = keep_view(a.sparse->indicesBufferView);
            auto vv = keep_view(a.sparse->valuesBufferView);
            if (!iv || !vv) return fail("sparse accessor " + std::to_string(i) + " out of range");
            a.sparse->indicesBufferView = *iv;
            a.sparse->valuesBufferView = *vv;
        }
    }

    for (std::size_t i = 0; i < asset.images.size(); ++i) {
        fastgltf::Image& img = asset.images[i];
        fastgltf::BufferView v{};
        fastgltf::MimeType mime = fastgltf::MimeType::KTX2;
        if (images[i]) {
            append_aligned(out.buffer, *images[i], v.byteOffset);
            v.byteLength = images[i]->size();
        } else {
            auto bytes = detail::source_bytes(asset, img.data);
            if (!bytes) return fail("image " + std::to_string(i) + ": " + bytes.error().message);
            std::visit(fastgltf::visitor{[&](const fastgltf::sources::BufferView& s) { mime = s.mimeType; },
                                         [&](const fastgltf::sources::Array& s) { mime = s.mimeType; },
                                         [&](const fastgltf::sources::Vector& s) { mime = s.mimeType; },
                                         [&](const auto&) {}},
                       img.data);
            append_aligned(out.buffer, *bytes, v.byteOffset);
            v.byteLength = bytes->size();
        }
        out.views.push_back(std::move(v));
        img.data = fastgltf::sources::BufferView{out.views.size() - 1, mime};
    }
    for (fastgltf::Texture& tex : asset.textures) {
        if (tex.imageIndex && images[*tex.imageIndex]) {
            tex.basisuImageIndex = tex.imageIndex;
            tex.imageIndex.reset();
        }
    }

    stats.mesh_bytes_before = geo.bytes_before;
    stats.mesh_bytes_after = mesh_after;
    out.buffer.resize((out.buffer.size() + 3) / 4 * 4);
    asset.buffers.clear();
    fastgltf::Buffer main{};
    main.byteLength = out.buffer.size();
    main.data = fastgltf::sources::Vector{std::move(out.buffer), fastgltf::MimeType::None};
    asset.buffers.push_back(std::move(main));
    if (out.fallback_bytes > 0) {
        fastgltf::Buffer fallback{};
        fallback.byteLength = out.fallback_bytes;
        fallback.data = fastgltf::sources::Fallback{};
        asset.buffers.push_back(std::move(fallback));
        require_extension(asset, "EXT_meshopt_compression");
    }
    asset.bufferViews = std::move(out.views);
    if (geo.quantized) require_extension(asset, "KHR_mesh_quantization");
    if (std::any_of(images.begin(), images.end(), [](const auto& i) { return i.has_value(); })) {
        require_extension(asset, "KHR_texture_basisu");
    }
    return {};
}

std::optional<std::string> mesh_extras(std::size_t index, fastgltf::Category category,
                                       void* user) {
    const auto& extras = *static_cast<const std::vector<std::string>*>(user);
    if (category != fastgltf::Category::Meshes || index >= extras.size() || extras[index].empty()) {
        return std::nullopt;
    }
    return extras[index];
}

[[nodiscard]] std::uint64_t file_bytes(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

} // namespace

std::expected<CookStats, core::Error> cook_gltf(const fs::path& input, const fs::path& output,
                                                const CookOptions& options) {
    auto data = fastgltf::GltfDataBuffer::FromPath(input);
    if (data.error() != fastgltf::Error::None) return fail("cannot read '" + input.string() + "'");
    fastgltf::Parser parser(k_extensions);
    auto parsed = parser.loadGltf(data.get(), input.parent_path(),
                                  fastgltf::Options::LoadExternalBuffers |
                                      fastgltf::Options::LoadExternalImages);
    if (parsed.error() != fastgltf::Error::None) {
        return fail(std::string("parse failed: ") +
                    std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    fastgltf::Asset asset = std::move(parsed.get());
    if (uses_extension(asset, "EXT_meshopt_compression")) {
        return fail("input already uses EXT_meshopt_compression; cook from the source asset");
    }
    if (auto ok = detail::check_ranges(asset); !ok) return std::unexpected(ok.error());

    CookStats stats;
    stats.input_bytes = file_bytes(input);
    if (input.extension() != ".glb") { // a .gltf's buffers / images are separate or base64
        for (const fastgltf::Buffer& b : asset.buffers) stats.input_bytes += b.byteLength;
        for (const fastgltf::Image& img : asset.images) {
            if (!std::holds_alternative<fastgltf::sources::BufferView>(img.data)) {
                if (auto b = detail::source_bytes(asset, img.data)) stats.input_bytes += b->size();
            }
        }
    }

    auto images = cook_images(asset, options.textures, stats);
    if (!images) return std::unexpected(images.error());
    auto geo = detail::cook_meshes(asset, options.meshes);
    if (!geo) return std::unexpected(geo.error());
    stats.primitives_cooked = geo->cooked;
    stats.primitives_skipped = geo->skipped;
    stats.lods_written = geo->lods;
    if (auto ok = assemble(asset, *geo, *images, options.meshes, stats); !ok) {
        return std::unexpected(ok.error());
    }

    fastgltf::FileExporter exporter;
    exporter.setUserPointer(&geo->mesh_extras);
    exporter.setExtrasWriteCallback(&mesh_extras);
    const bool binary = output.extension() == ".glb";
    if (!binary) asset.buffers[0].name = output.stem().string();
    const fastgltf::Error written = binary ? exporter.writeGltfBinary(asset, output)
                                           : exporter.writeGltfJson(asset, output);
    if (written != fastgltf::Error::None) {
        return fail("writing '" + output.string() + "' failed: " +
                    std::string(fastgltf::getErrorMessage(written)));
    }
    stats.output_bytes = file_bytes(output);
    if (!binary) stats.output_bytes += file_bytes(output.parent_path() / (output.stem().string() + ".bin"));
    return stats;
}

} // namespace engine::cook
