// glTF mesh extraction and upload.

#define GLM_ENABLE_EXPERIMENTAL

#include "engine/scene/gltf_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "engine/core/utf8_path.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "scene/gltf/internal.hpp"

namespace engine::scene {

namespace gltf_detail {

namespace {

// Appends a primitive's triangle indices (offset by base_vertex). Primitives
// without an index accessor get the implicit sequential list 0..n-1 — built
// here, after validate_asset(), rather than by fastgltf's
// Options::GenerateMeshIndices, which runs during parsing and dereferences the
// POSITION accessor index before anything has range-checked it (an
// out-of-bounds read on malformed files, found by valgrind).
[[nodiscard]] std::expected<void, core::Error>
append_indices(const fastgltf::Asset& asset, const fastgltf::Primitive& prim,
               std::size_t vertex_count, std::uint32_t base_vertex,
               std::vector<std::uint32_t>& out) {
    if (!prim.indicesAccessor.has_value()) {
        if (vertex_count % 3 != 0) return fail("glTF: non-indexed triangle list size not a multiple of 3");
        out.reserve(out.size() + vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            out.push_back(base_vertex + static_cast<std::uint32_t>(i));
        }
        return {};
    }
    const fastgltf::Accessor& idx_acc = asset.accessors[prim.indicesAccessor.value()];
    out.reserve(out.size() + idx_acc.count);
    bool index_out_of_range = false;
    fastgltf::iterateAccessor<std::uint32_t>(asset, idx_acc, [&](std::uint32_t idx) {
        index_out_of_range = index_out_of_range || idx >= vertex_count;
        out.push_back(base_vertex + idx);
    });
    // An index past the vertex count makes the GPU fetch out of bounds.
    if (index_out_of_range) return fail("glTF: vertex index out of range");
    return {};
}

} // namespace


// Merge every triangle primitive of every mesh into one vertex/index stream.
// Indices are rebased per primitive so the merged index buffer is self-contained.
[[nodiscard]] std::expected<MeshData, core::Error> extract(const fastgltf::Asset& asset) {
    MeshData out;
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(std::numeric_limits<float>::lowest());

    for (const fastgltf::Mesh& mesh : asset.meshes) {
        for (const fastgltf::Primitive& prim : mesh.primitives) {
            if (prim.type != fastgltf::PrimitiveType::Triangles) continue;

            const auto* pos_attr = prim.findAttribute("POSITION");
            if (pos_attr == prim.attributes.cend()) {
                return fail("glTF primitive missing required POSITION attribute");
            }

            const auto base_vertex = static_cast<std::uint32_t>(out.vertices.size());
            const fastgltf::Accessor& pos_acc = asset.accessors[pos_attr->accessorIndex];
            out.vertices.resize(out.vertices.size() + pos_acc.count);

            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, pos_acc, [&](fastgltf::math::fvec3 v, std::size_t i) {
                    const glm::vec3 p = to_glm(v);
                    out.vertices[base_vertex + i].position = p;
                    bmin = glm::min(bmin, p);
                    bmax = glm::max(bmax, p);
                });

            if (const auto* a = prim.findAttribute("NORMAL"); a != prim.attributes.cend()) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, asset.accessors[a->accessorIndex],
                    [&](fastgltf::math::fvec3 v, std::size_t i) {
                        out.vertices[base_vertex + i].normal = to_glm(v);
                    });
            }
            if (const auto* a = prim.findAttribute("TEXCOORD_0"); a != prim.attributes.cend()) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                    asset, asset.accessors[a->accessorIndex],
                    [&](fastgltf::math::fvec2 v, std::size_t i) {
                        out.vertices[base_vertex + i].uv = to_glm(v);
                    });
            }
            if (const auto* a = prim.findAttribute("TANGENT"); a != prim.attributes.cend()) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                    asset, asset.accessors[a->accessorIndex],
                    [&](fastgltf::math::fvec4 v, std::size_t i) {
                        out.vertices[base_vertex + i].tangent = to_glm(v);
                    });
            }

            const auto index_offset = static_cast<std::uint32_t>(out.indices.size());
            if (auto ok = append_indices(asset, prim, pos_acc.count, base_vertex, out.indices);
                !ok) {
                return std::unexpected(ok.error());
            }

            asset::SubMesh sub;
            sub.index_offset = index_offset;
            sub.index_count = static_cast<std::uint32_t>(out.indices.size() - index_offset);
            sub.material = prim.materialIndex.has_value()
                               ? static_cast<std::uint32_t>(prim.materialIndex.value())
                               : 0U;
            out.submeshes.push_back(sub);
        }
    }

    if (out.vertices.empty() || out.indices.empty()) {
        return fail("glTF contained no triangle geometry");
    }
    out.bounds = {bmin, bmax};
    return out;
}

std::optional<std::uint32_t> max_weighted_joint(const fastgltf::Asset& asset,
                                                const fastgltf::Primitive& prim) {
    const auto* joints_attr = prim.findAttribute("JOINTS_0");
    const auto* weights_attr = prim.findAttribute("WEIGHTS_0");
    if (joints_attr == prim.attributes.cend() || weights_attr == prim.attributes.cend()) {
        return std::nullopt;
    }
    const fastgltf::Accessor& joints_acc = asset.accessors[joints_attr->accessorIndex];
    std::vector<glm::vec4> weights(joints_acc.count, glm::vec4(0.0F));
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
        asset, asset.accessors[weights_attr->accessorIndex],
        [&](fastgltf::math::fvec4 v, std::size_t i) {
            if (i < weights.size()) weights[i] = to_glm(v);
        });
    std::optional<std::uint32_t> max_joint;
    fastgltf::iterateAccessorWithIndex<fastgltf::math::uvec4>(
        asset, joints_acc, [&](fastgltf::math::uvec4 v, std::size_t i) {
            for (std::size_t k = 0; k < 4; ++k) {
                // NaN weights count as used: they poison the blend just the same.
                if (!(weights[i][static_cast<glm::length_t>(k)] == 0.0F)) {
                    const auto j = static_cast<std::uint32_t>(v[k]);
                    max_joint = max_joint ? std::max(*max_joint, j) : j;
                }
            }
        });
    return max_joint;
}

// Extract a single triangle primitive into its own self-contained MeshData (one
// submesh spanning all its indices). Used by scene instantiation so each glTF
// primitive becomes one MeshAsset with its own material — the granularity the
// one-material-per-draw renderer needs.
[[nodiscard]] std::expected<MeshData, core::Error>
extract_primitive(const fastgltf::Asset& asset, const fastgltf::Primitive& prim) {
    if (prim.type != fastgltf::PrimitiveType::Triangles) {
        return fail("glTF primitive is not a triangle list");
    }
    const auto* pos_attr = prim.findAttribute("POSITION");
    if (pos_attr == prim.attributes.cend()) {
        return fail("glTF primitive missing required POSITION attribute");
    }

    MeshData out;
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(std::numeric_limits<float>::lowest());

    const fastgltf::Accessor& pos_acc = asset.accessors[pos_attr->accessorIndex];
    out.vertices.resize(pos_acc.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, pos_acc, [&](fastgltf::math::fvec3 v, std::size_t i) {
            const glm::vec3 p = to_glm(v);
            out.vertices[i].position = p;
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
        });
    if (const auto* a = prim.findAttribute("NORMAL"); a != prim.attributes.cend()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            asset, asset.accessors[a->accessorIndex],
            [&](fastgltf::math::fvec3 v, std::size_t i) { out.vertices[i].normal = to_glm(v); });
    }
    if (const auto* a = prim.findAttribute("TEXCOORD_0"); a != prim.attributes.cend()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
            asset, asset.accessors[a->accessorIndex],
            [&](fastgltf::math::fvec2 v, std::size_t i) { out.vertices[i].uv = to_glm(v); });
    }
    if (const auto* a = prim.findAttribute("TANGENT"); a != prim.attributes.cend()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            asset, asset.accessors[a->accessorIndex],
            [&](fastgltf::math::fvec4 v, std::size_t i) { out.vertices[i].tangent = to_glm(v); });
    }

    // Skin influences (parallel to vertices), if this primitive is skinned.
    const auto* joints_attr = prim.findAttribute("JOINTS_0");
    const auto* weights_attr = prim.findAttribute("WEIGHTS_0");
    if (joints_attr != prim.attributes.cend() && weights_attr != prim.attributes.cend()) {
        out.skin.resize(out.vertices.size());
        fastgltf::iterateAccessorWithIndex<fastgltf::math::uvec4>(
            asset, asset.accessors[joints_attr->accessorIndex],
            [&](fastgltf::math::uvec4 v, std::size_t i) {
                out.skin[i].joints = glm::uvec4(v[0], v[1], v[2], v[3]);
            });
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            asset, asset.accessors[weights_attr->accessorIndex],
            [&](fastgltf::math::fvec4 v, std::size_t i) { out.skin[i].weights = to_glm(v); });
    }

    if (auto ok = append_indices(asset, prim, pos_acc.count, 0, out.indices); !ok) {
        return std::unexpected(ok.error());
    }

    if (out.vertices.empty() || out.indices.empty()) {
        return fail("glTF primitive contained no triangle geometry");
    }
    out.bounds = {bmin, bmax};
    out.submeshes = {asset::SubMesh{0, static_cast<std::uint32_t>(out.indices.size()), 0}};
    return out;
}


[[nodiscard]] std::expected<MeshData, core::Error>
parse_and_extract(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir) {
    fastgltf::Parser parser(kParserExtensions);
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data, base_dir, kLoadOptions);
    if (asset.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(asset.error())));
    }
    if (auto valid = validate_asset(asset.get()); !valid) return std::unexpected(valid.error());
    return extract(asset.get());
}

// -------------------------------------------------------------------------
// Textures
// -------------------------------------------------------------------------


} // namespace gltf_detail

using namespace gltf_detail;


std::expected<MeshData, core::Error>
GltfLoader::load_mesh_data(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + core::path_to_utf8(path) + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract(data.get(), path.parent_path());
}

std::expected<MeshData, core::Error>
GltfLoader::load_mesh_data_from_memory(std::span<const std::byte> bytes,
                                       const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract(data.get(), base_dir);
}

std::expected<asset::MeshAsset, core::Error>
upload_mesh(const MeshData& data, rhi::GpuAllocator& allocator,
            const rhi::TransferContext& transfer) {
    asset::MeshAsset mesh;
    const bool skinned = !data.skin.empty();

    // A skinned mesh's vertex buffer is also read by the skinning compute pass as
    // a storage buffer (via its device address).
    VkBufferUsageFlags vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (skinned) {
        vertex_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    auto vertex_buffer = rhi::upload_device_buffer(
        allocator, transfer, data.vertices.data(),
        data.vertices.size() * sizeof(asset::Vertex), vertex_usage);
    if (!vertex_buffer) return std::unexpected(vertex_buffer.error());

    auto index_buffer = rhi::upload_device_buffer(
        allocator, transfer, data.indices.data(),
        data.indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!index_buffer) return std::unexpected(index_buffer.error());

    mesh.vertex_buffer = std::move(*vertex_buffer);
    mesh.index_buffer = std::move(*index_buffer);

    if (skinned) {
        auto skin_buffer = rhi::upload_device_buffer(
            allocator, transfer, data.skin.data(), data.skin.size() * sizeof(asset::SkinVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        if (!skin_buffer) return std::unexpected(skin_buffer.error());
        mesh.skin_buffer = std::move(*skin_buffer);
    }

    mesh.vertex_count = static_cast<std::uint32_t>(data.vertices.size());
    mesh.index_count = static_cast<std::uint32_t>(data.indices.size());
    mesh.bounds = data.bounds;
    mesh.submeshes = data.submeshes;
    return mesh;
}

std::expected<asset::MeshAsset, core::Error>
GltfLoader::load(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                 const rhi::TransferContext& transfer) {
    auto data = load_mesh_data(path);
    if (!data) return std::unexpected(data.error());
    return upload_mesh(*data, allocator, transfer);
}

} // namespace engine::scene
