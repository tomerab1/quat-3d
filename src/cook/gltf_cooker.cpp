// glTF cook: parse + validate (the runtime's parse_gltf) -> meshes
// (mesh_cook.cpp) -> images (KTX2) -> one compacted binary buffer plus an
// EXT_meshopt_compression fallback buffer -> export.

#include <algorithm>
#include <cstring>
#include <string>
#include <variant>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <meshoptimizer.h>

#include "cook/internal.hpp"
#include "engine/asset/image_decode.hpp"

namespace engine::cook {

namespace {

using detail::fail;
namespace fs = std::filesystem;
namespace gltf = scene::gltf_detail;

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

void drop_extension(fastgltf::Asset& asset, std::string_view name) {
    const auto is = [&](const auto& e) { return std::string_view(e) == name; };
    std::erase_if(asset.extensionsUsed, is);
    std::erase_if(asset.extensionsRequired, is);
}

struct CookedImages {
    std::vector<std::optional<std::vector<std::byte>>> ktx2; // nullopt: kept as is
    std::vector<bool>                                  normal_xy;
};

// Encodes every sampled PNG/JPEG image to KTX2 (decoded through the runtime's
// guarded decoder and limits); other images (KTX2 already) are kept.
[[nodiscard]] std::expected<CookedImages, core::Error>
cook_images(const fastgltf::Asset& asset, const TextureCookOptions& o, CookStats& stats) {
    CookedImages out{std::vector<std::optional<std::vector<std::byte>>>(asset.images.size()),
                     std::vector<bool>(asset.images.size(), false)};
    if (!o.enabled) return out;
    const std::vector<gltf::ImageUsage> uses = gltf::image_usages(asset);
    asset::DecodeBudget budget(asset::TextureLimits{}.max_total_decoded_bytes);
    for (std::size_t i = 0; i < asset.images.size(); ++i) {
        if (!uses[i].sampled) continue;
        auto encoded = gltf::data_source_bytes(asset, asset.images[i].data);
        if (!encoded) return std::unexpected(encoded.error());
        if (!asset::is_png_or_jpeg(*encoded)) continue;
        auto image = asset::decode_png_jpeg(*encoded, uses[i].color_space, {}, &budget);
        if (!image) return fail("image " + std::to_string(i) + ": " + image.error().message);
        auto ktx2 = detail::encode_image_ktx2(image->pixels.span(), image->width, image->height,
                                              uses[i].role, uses[i].color_space, o);
        if (!ktx2) return fail("image " + std::to_string(i) + ": " + ktx2.error().message);
        stats.image_bytes_before += encoded->size();
        stats.image_bytes_after += ktx2->size();
        ++stats.images_encoded;
        out.ktx2[i] = std::move(*ktx2);
        out.normal_xy[i] = uses[i].role == asset::TextureRole::normal && o.normal_xy_layout;
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
         const CookedImages& cooked, const MeshCookOptions& o, CookStats& stats,
         std::vector<std::string>& texture_extras) {
    const auto& images = cooked.ktx2;
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
            auto bytes = gltf::data_source_bytes(asset, img.data);
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
    // Cooked images replace the PNG/JPEG source outright (no fallback);
    // normal maps in the X-in-RGB / Y-in-alpha layout are marked so tools
    // other than the engine can tell (the engine reads the KTX2 descriptor).
    texture_extras.assign(asset.textures.size(), std::string{});
    for (std::size_t t = 0; t < asset.textures.size(); ++t) {
        fastgltf::Texture& tex = asset.textures[t];
        if (tex.imageIndex && images[*tex.imageIndex]) {
            if (cooked.normal_xy[*tex.imageIndex]) texture_extras[t] = R"({"quat_normal_xy":true})";
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
    }
    // Compressed input was decoded at parse; only this cook's views use it now.
    drop_extension(asset, "EXT_meshopt_compression");
    if (out.fallback_bytes > 0) require_extension(asset, "EXT_meshopt_compression");
    asset.bufferViews = std::move(out.views);
    if (geo.quantized) require_extension(asset, "KHR_mesh_quantization");
    if (std::any_of(images.begin(), images.end(), [](const auto& i) { return i.has_value(); })) {
        require_extension(asset, "KHR_texture_basisu");
    }
    return {};
}

std::optional<std::string> texture_extras(std::size_t index, fastgltf::Category category,
                                          void* user) {
    const auto& extras = *static_cast<const std::vector<std::string>*>(user);
    if (category != fastgltf::Category::Textures || index >= extras.size() ||
        extras[index].empty()) {
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
    // The runtime's own parse: extensions, EXT_meshopt_compression decoding
    // and validate_asset() (buffer / accessor ranges, references, hierarchy).
    auto parsed = gltf::parse_gltf(data.get(), input.parent_path());
    if (!parsed) return fail(parsed.error().message);
    fastgltf::Asset asset = std::move(*parsed);

    CookStats stats;
    stats.input_bytes = file_bytes(input);
    if (input.extension() != ".glb") { // a .gltf's buffers / images are separate or base64
        for (const fastgltf::Buffer& b : asset.buffers) { // decoded meshopt views are Vectors
            if (!std::holds_alternative<fastgltf::sources::Vector>(b.data)) {
                stats.input_bytes += b.byteLength;
            }
        }
        for (const fastgltf::Image& img : asset.images) {
            if (!std::holds_alternative<fastgltf::sources::BufferView>(img.data)) {
                if (auto b = gltf::data_source_bytes(asset, img.data)) stats.input_bytes += b->size();
            }
        }
    }

    auto images = cook_images(asset, options.textures, stats);
    if (!images) return std::unexpected(images.error());
    auto geo = detail::cook_meshes(asset, options.meshes);
    if (!geo) return std::unexpected(geo.error());
    stats.primitives_cooked = geo->cooked;
    stats.primitives_skipped = geo->skipped;
    std::vector<std::string> extras;
    if (auto ok = assemble(asset, *geo, *images, options.meshes, stats, extras); !ok) {
        return std::unexpected(ok.error());
    }

    fastgltf::FileExporter exporter;
    exporter.setUserPointer(&extras);
    exporter.setExtrasWriteCallback(&texture_extras);
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
