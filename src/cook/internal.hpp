#pragma once

// Private to the cooker (src/cook/*.cpp).
//
//   texture_encode.cpp  RGBA8 -> mip chain -> KTX2 / Basis Universal
//   mesh_cook.cpp       primitive grouping, meshoptimizer passes
//   mesh_simplify.cpp   optional level-of-detail reduction (--simplify)
//   mesh_streams.cpp    attribute quantisation into new buffer views
//   gltf_access.cpp     buffer view bytes
//   gltf_cooker.cpp     parse (runtime parse_gltf), textures, buffer compaction,
//                       export

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fastgltf/types.hpp>

#include "engine/cook/gltf_cooker.hpp"
#include "engine/core/error.hpp"
#include "scene/gltf/gltf_io.hpp"

namespace engine::cook::detail {

[[nodiscard]] inline std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"quat-cook: " + std::move(what)});
}

// A buffer view the cook creates: `raw` holds count * stride bytes in the
// layout the loader sees after decoding; when compressed, the meshopt codec
// (and filter) are applied at buffer assembly.
struct PendingView {
    std::vector<std::byte>             raw;
    std::size_t                        count  = 0;
    std::size_t                        stride = 0;
    bool                               vertex = true; // byteStride written; else indices
    fastgltf::MeshoptCompressionMode   mode   = fastgltf::MeshoptCompressionMode::Attributes;
    fastgltf::MeshoptCompressionFilter filter = fastgltf::MeshoptCompressionFilter::None;
    // Filter-encoded payload when `filter` is set (raw keeps the decoded form).
    std::vector<std::byte>             filtered;
};

struct GeometryResult {
    std::vector<PendingView> views;
    // Per accessor (asset.accessors, including appended index accessors):
    // the pending view it now reads from.
    std::vector<std::optional<std::size_t>> accessor_view;
    bool          quantized = false; // any attribute needs KHR_mesh_quantization
    std::uint32_t cooked = 0;
    std::uint32_t skipped = 0;
    std::uint64_t bytes_before = 0;
    std::uint64_t bytes_after = 0; // raw (pre-codec) bytes of the new views
};

// gltf_access.cpp: bytes of a buffer view, range-checked.
[[nodiscard]] std::expected<std::span<const std::byte>, core::Error>
view_bytes(const fastgltf::Asset& asset, std::size_t view);

// mesh_cook.cpp: rewrites the accessors of every cookable triangle primitive
// group in place (they now point at GeometryResult::views).
[[nodiscard]] std::expected<GeometryResult, core::Error>
cook_meshes(fastgltf::Asset& asset, const MeshCookOptions& options);

// mesh_streams.cpp: one de-interleaved vertex attribute after optimisation.
struct Stream {
    std::string        name;       // glTF semantic, e.g. "TEXCOORD_0"
    std::size_t        accessor;   // accessor rewritten to read the new view
    std::size_t        components; // 1..4
    std::vector<float> values;     // vertex_count * components
};

// Writes `stream` as a new pending view and points its accessor at it.
void emit_stream(fastgltf::Asset& asset, const Stream& stream, std::size_t vertex_count,
                 const MeshCookOptions& options, GeometryResult& out);

// Writes `indices` as a new pending view read by accessor `accessor`.
void emit_indices(fastgltf::Asset& asset, std::size_t accessor,
                  std::span<const std::uint32_t> indices, std::size_t vertex_count,
                  const MeshCookOptions& options, GeometryResult& out);

// mesh_simplify.cpp: reduces `indices` (one primitive over the vertices of
// `positions`) to about options.simplify_ratio of its triangles; returns the
// new index count (unchanged when simplification is off or would remove
// every triangle). Thinning disconnected foliage grows the kept cards, which
// moves their vertices in `positions` — except vertices flagged in `shared`
// (used by another primitive too). `normals` / `uvs` may be null.
std::size_t simplify_triangles(std::vector<std::uint32_t>& indices, Stream& positions,
                               const Stream* normals, const Stream* uvs,
                               std::span<const std::uint8_t> shared,
                               const MeshCookOptions& options);

// texture_encode.cpp: an RGBA8 image's mip chain encoded to KTX2.
[[nodiscard]] std::expected<std::vector<std::byte>, core::Error>
encode_image_ktx2(std::span<const std::byte> rgba, std::uint32_t width, std::uint32_t height,
                  asset::TextureRole role, asset::TextureColorSpace color_space,
                  const TextureCookOptions& options);

} // namespace engine::cook::detail
