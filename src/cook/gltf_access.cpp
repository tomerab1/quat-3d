// Range-checked buffer view access for the cooker (on the runtime's
// data_source_bytes; the asset was validated by parse_gltf).

#include <string>
#include <variant>

#include "cook/internal.hpp"

namespace engine::cook::detail {

std::expected<std::span<const std::byte>, core::Error> view_bytes(const fastgltf::Asset& asset,
                                                                  std::size_t view_index) {
    if (view_index >= asset.bufferViews.size()) return fail("bufferView index out of range");
    const fastgltf::BufferView& view = asset.bufferViews[view_index];
    if (view.bufferIndex >= asset.buffers.size()) return fail("buffer index out of range");
    auto bytes = scene::gltf_detail::data_source_bytes(asset, asset.buffers[view.bufferIndex].data);
    if (!bytes) return bytes;
    if (view.byteOffset > bytes->size() || view.byteLength > bytes->size() - view.byteOffset) {
        return fail("bufferView " + std::to_string(view_index) + " exceeds its buffer");
    }
    return bytes->subspan(view.byteOffset, view.byteLength);
}

} // namespace engine::cook::detail
