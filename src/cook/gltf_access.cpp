// Range-checked access to a parsed glTF's buffers for the cooker.

#include <string>
#include <variant>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include "cook/internal.hpp"

namespace engine::cook::detail {

namespace {

using Bytes = std::expected<std::span<const std::byte>, core::Error>;

[[nodiscard]] bool in_range(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}

} // namespace

Bytes source_bytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source) {
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& a) -> Bytes {
                return std::span<const std::byte>(a.bytes.data(), a.bytes.size());
            },
            [](const fastgltf::sources::Vector& v) -> Bytes {
                return std::span<const std::byte>(v.bytes.data(), v.bytes.size());
            },
            [](const fastgltf::sources::ByteView& b) -> Bytes { return b.bytes; },
            [&](const fastgltf::sources::BufferView& bv) -> Bytes {
                return view_bytes(asset, bv.bufferViewIndex);
            },
            [](const auto&) -> Bytes { return fail("data source not loaded"); }},
        source);
}

Bytes view_bytes(const fastgltf::Asset& asset, std::size_t view_index) {
    if (view_index >= asset.bufferViews.size()) return fail("bufferView index out of range");
    const fastgltf::BufferView& view = asset.bufferViews[view_index];
    if (view.bufferIndex >= asset.buffers.size()) return fail("buffer index out of range");
    const fastgltf::DataSource& data = asset.buffers[view.bufferIndex].data;
    if (std::holds_alternative<fastgltf::sources::BufferView>(data)) {
        return fail("buffer data cannot be a bufferView");
    }
    auto bytes = source_bytes(asset, data);
    if (!bytes) return bytes;
    if (!in_range(view.byteOffset, view.byteLength, bytes->size())) {
        return fail("bufferView " + std::to_string(view_index) + " exceeds its buffer");
    }
    return bytes->subspan(view.byteOffset, view.byteLength);
}

std::expected<void, core::Error> check_ranges(const fastgltf::Asset& asset) {
    for (std::size_t v = 0; v < asset.bufferViews.size(); ++v) {
        if (auto b = view_bytes(asset, v); !b) return std::unexpected(b.error());
    }
    const auto holds = [&](std::size_t view, std::size_t offset, std::size_t count,
                           std::size_t elem, std::size_t stride) {
        if (view >= asset.bufferViews.size()) return false;
        if (count == 0) return true;
        const std::size_t length = asset.bufferViews[view].byteLength;
        if (stride < elem || count - 1 > length / stride) return false;
        return in_range(offset, (count - 1) * stride + elem, length);
    };
    for (std::size_t i = 0; i < asset.accessors.size(); ++i) {
        const fastgltf::Accessor& a = asset.accessors[i];
        const std::size_t elem = fastgltf::getElementByteSize(a.type, a.componentType);
        bool ok = elem != 0;
        if (ok && a.bufferViewIndex) {
            const std::size_t view = *a.bufferViewIndex;
            const std::size_t stride =
                view < asset.bufferViews.size() ? asset.bufferViews[view].byteStride.value_or(elem)
                                                : elem;
            ok = holds(view, a.byteOffset, a.count, elem, stride);
        }
        if (ok && a.sparse) {
            const std::size_t isz = fastgltf::getComponentByteSize(a.sparse->indexComponentType);
            ok = isz != 0 && a.sparse->count <= a.count &&
                 holds(a.sparse->indicesBufferView, a.sparse->indicesByteOffset, a.sparse->count,
                       isz, isz) &&
                 holds(a.sparse->valuesBufferView, a.sparse->valuesByteOffset, a.sparse->count,
                       elem, elem);
        }
        if (!ok) return fail("accessor " + std::to_string(i) + " lies outside its buffer data");
    }
    const std::size_t n_acc = asset.accessors.size();
    for (const fastgltf::Mesh& mesh : asset.meshes) {
        for (const fastgltf::Primitive& prim : mesh.primitives) {
            bool ok = !prim.indicesAccessor || *prim.indicesAccessor < n_acc;
            for (const auto& attr : prim.attributes) ok = ok && attr.accessorIndex < n_acc;
            for (const auto& target : prim.targets) {
                for (const auto& attr : target) ok = ok && attr.accessorIndex < n_acc;
            }
            if (!ok) return fail("primitive accessor index out of range");
        }
    }
    for (const fastgltf::Texture& tex : asset.textures) {
        if ((tex.imageIndex && *tex.imageIndex >= asset.images.size()) ||
            (tex.basisuImageIndex && *tex.basisuImageIndex >= asset.images.size())) {
            return fail("texture image index out of range");
        }
    }
    return {};
}

} // namespace engine::cook::detail
