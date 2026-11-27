// EXT_meshopt_compression: decode compressed buffer views at load.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <fastgltf/types.hpp>
#include <meshoptimizer.h>

#include "scene/gltf/gltf_io.hpp"

namespace engine::scene::gltf_detail {

namespace {

// Decoded size cap per view: a hostile count * stride must not drive a huge
// allocation before the decoder looks at a single byte.
constexpr std::size_t k_max_decoded_view_bytes = std::size_t{1} << 30;

// Densest the meshopt codecs get: a constant attribute stream costs a 2-bit
// header per 16 bytes (v0) or a control bit pair per byte column of a
// 256-vertex block (v1), so 1024:1 plus a small fixed tail bounds any
// legitimate view; a claim beyond it is a decompression bomb.
constexpr std::uint64_t k_max_expansion = 1024;
constexpr std::uint64_t k_expansion_slack = 4096;

[[nodiscard]] std::expected<void, core::Error>
check_layout(const fastgltf::CompressedBufferView& c, std::size_t view_index) {
    const std::string where = "glTF: EXT_meshopt_compression bufferView " +
                              std::to_string(view_index) + ": ";
    using Mode = fastgltf::MeshoptCompressionMode;
    using Filter = fastgltf::MeshoptCompressionFilter;
    switch (c.mode) {
        case Mode::Attributes:
            if (c.byteStride == 0 || c.byteStride % 4 != 0 || c.byteStride > 256) {
                return fail(where + "attribute stride must be a multiple of 4 up to 256");
            }
            break;
        case Mode::Triangles:
            if (c.count % 3 != 0) return fail(where + "triangle index count not a multiple of 3");
            [[fallthrough]];
        case Mode::Indices:
            if (c.byteStride != 2 && c.byteStride != 4) {
                return fail(where + "index stride must be 2 or 4");
            }
            break;
    }
    if (c.filter != Filter::None && c.mode != Mode::Attributes) {
        return fail(where + "filters apply to attribute data only");
    }
    if ((c.filter == Filter::Octahedral && c.byteStride != 4 && c.byteStride != 8) ||
        (c.filter == Filter::Quaternion && c.byteStride != 8)) {
        return fail(where + "stride does not match the filter");
    }
    if (c.count > k_max_decoded_view_bytes / c.byteStride) {
        return fail(where + "decoded size exceeds the limit");
    }
    return {};
}

[[nodiscard]] bool decode_view(const fastgltf::CompressedBufferView& c,
                               std::span<const std::byte> src, std::vector<std::byte>& dst) {
    const auto* in = reinterpret_cast<const unsigned char*>(src.data());
    int rc = -1;
    switch (c.mode) {
        case fastgltf::MeshoptCompressionMode::Attributes:
            rc = meshopt_decodeVertexBuffer(dst.data(), c.count, c.byteStride, in, src.size());
            break;
        case fastgltf::MeshoptCompressionMode::Triangles:
            rc = meshopt_decodeIndexBuffer(dst.data(), c.count, c.byteStride, in, src.size());
            break;
        case fastgltf::MeshoptCompressionMode::Indices:
            rc = meshopt_decodeIndexSequence(dst.data(), c.count, c.byteStride, in, src.size());
            break;
    }
    if (rc != 0) return false;
    switch (c.filter) {
        case fastgltf::MeshoptCompressionFilter::None: break;
        case fastgltf::MeshoptCompressionFilter::Octahedral:
            meshopt_decodeFilterOct(dst.data(), c.count, c.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Quaternion:
            meshopt_decodeFilterQuat(dst.data(), c.count, c.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Exponential:
            meshopt_decodeFilterExp(dst.data(), c.count, c.byteStride);
            break;
    }
    return true;
}

} // namespace

std::expected<void, core::Error> decode_meshopt_buffers(fastgltf::Asset& asset,
                                                        std::uint64_t max_total_bytes) {
    const std::size_t source_buffers = asset.buffers.size();
    // Pass 1, before anything is allocated: every compressed view's layout,
    // and the decoded size of all of them together against the asset limit.
    std::uint64_t total = 0;
    for (std::size_t vi = 0; vi < asset.bufferViews.size(); ++vi) {
        const fastgltf::BufferView& view = asset.bufferViews[vi];
        if (view.meshoptCompression == nullptr) continue;
        const fastgltf::CompressedBufferView& c = *view.meshoptCompression;
        if (auto ok = check_layout(c, vi); !ok) return ok;
        const std::uint64_t decoded_bytes = c.count * c.byteStride;
        if (decoded_bytes != view.byteLength) {
            return fail("glTF: EXT_meshopt_compression bufferView " + std::to_string(vi) +
                        " decoded size differs from its byteLength");
        }
        if (decoded_bytes > max_total_bytes - total) {
            return fail("glTF: EXT_meshopt_compression data decodes past the asset's limit of " +
                        std::to_string(max_total_bytes) + " bytes");
        }
        total += decoded_bytes;
    }

    // Pass 2: source ranges, plausibility, decode.
    for (std::size_t vi = 0; vi < asset.bufferViews.size(); ++vi) {
        fastgltf::BufferView& view = asset.bufferViews[vi];
        if (view.meshoptCompression == nullptr) continue;
        const fastgltf::CompressedBufferView& c = *view.meshoptCompression;
        if (c.bufferIndex >= source_buffers) {
            return fail("glTF: EXT_meshopt_compression buffer index out of range");
        }
        auto buffer = data_source_bytes(asset, asset.buffers[c.bufferIndex].data);
        if (!buffer) return std::unexpected(buffer.error());
        if (c.byteOffset > buffer->size() || c.byteLength > buffer->size() - c.byteOffset) {
            return fail("glTF: EXT_meshopt_compression source range outside its buffer");
        }
        const std::uint64_t decoded_bytes = c.count * c.byteStride;
        if (decoded_bytes > c.byteLength * k_max_expansion + k_expansion_slack) {
            return fail("glTF: EXT_meshopt_compression bufferView " + std::to_string(vi) +
                        " claims " + std::to_string(decoded_bytes) + " bytes from " +
                        std::to_string(c.byteLength) + " compressed bytes");
        }

        std::vector<std::byte> decoded(c.count * c.byteStride);
        if (!decode_view(c, buffer->subspan(c.byteOffset, c.byteLength), decoded)) {
            return fail("glTF: EXT_meshopt_compression bufferView " + std::to_string(vi) +
                        " failed to decode");
        }
        fastgltf::Buffer out;
        out.byteLength = decoded.size();
        out.data = fastgltf::sources::Vector{std::move(decoded), fastgltf::MimeType::None};
        view.bufferIndex = asset.buffers.size();
        view.byteOffset = 0;
        view.meshoptCompression.reset();
        asset.buffers.push_back(std::move(out));
    }
    return {};
}

} // namespace engine::scene::gltf_detail
