#include "engine/scene/gltf_loader.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/glm.hpp>

namespace engine::scene {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr auto kLoadOptions = fastgltf::Options::LoadExternalBuffers |
                              fastgltf::Options::GenerateMeshIndices;

[[nodiscard]] glm::vec3 to_glm(fastgltf::math::fvec3 v) { return {v[0], v[1], v[2]}; }
[[nodiscard]] glm::vec2 to_glm(fastgltf::math::fvec2 v) { return {v[0], v[1]}; }
[[nodiscard]] glm::vec4 to_glm(fastgltf::math::fvec4 v) { return {v[0], v[1], v[2], v[3]}; }

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

            // GenerateMeshIndices guarantees an index accessor even for
            // originally non-indexed primitives.
            if (!prim.indicesAccessor.has_value()) {
                return fail("glTF primitive has no index accessor after generation");
            }
            const fastgltf::Accessor& idx_acc = asset.accessors[prim.indicesAccessor.value()];
            const auto index_offset = static_cast<std::uint32_t>(out.indices.size());
            out.indices.reserve(out.indices.size() + idx_acc.count);
            fastgltf::iterateAccessor<std::uint32_t>(
                asset, idx_acc,
                [&](std::uint32_t idx) { out.indices.push_back(base_vertex + idx); });

            asset::SubMesh sub;
            sub.index_offset = index_offset;
            sub.index_count = static_cast<std::uint32_t>(idx_acc.count);
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

[[nodiscard]] std::expected<MeshData, core::Error>
parse_and_extract(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir) {
    fastgltf::Parser parser;
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data, base_dir, kLoadOptions);
    if (asset.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(asset.error())));
    }
    return extract(asset.get());
}

} // namespace

std::expected<MeshData, core::Error>
GltfLoader::load_mesh_data(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
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

    auto vertex_buffer = rhi::upload_device_buffer(
        allocator, transfer, data.vertices.data(),
        data.vertices.size() * sizeof(asset::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertex_buffer) return std::unexpected(vertex_buffer.error());

    auto index_buffer = rhi::upload_device_buffer(
        allocator, transfer, data.indices.data(),
        data.indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!index_buffer) return std::unexpected(index_buffer.error());

    mesh.vertex_buffer = std::move(*vertex_buffer);
    mesh.index_buffer = std::move(*index_buffer);
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

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] std::string base64_encode(std::span<const std::byte> in) {
    static constexpr char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto byte_at = [&](std::size_t i) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[i]));
    };
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const std::uint32_t n = (byte_at(i) << 16) | (byte_at(i + 1) << 8) | byte_at(i + 2);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += tbl[n & 63];
    }
    if (const std::size_t rem = in.size() - i; rem == 1) {
        const std::uint32_t n = byte_at(i) << 16;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        const std::uint32_t n = (byte_at(i) << 16) | (byte_at(i + 1) << 8);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += "=";
    }
    return out;
}

} // namespace

std::expected<void, core::Error>
run_gltf_loader_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    // One triangle: positions at offset 0 (3 x vec3), indices at offset 36
    // (3 x uint16). Positions first keeps the float data 4-byte aligned.
    const std::array<float, 9> positions{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    const std::array<std::uint16_t, 3> indices{0, 1, 2};

    std::array<std::byte, 42> buffer{};
    std::memcpy(buffer.data(), positions.data(), sizeof(positions));
    std::memcpy(buffer.data() + sizeof(positions), indices.data(), sizeof(indices));
    const std::string b64 = base64_encode(buffer);

    const std::string gltf =
        R"({"asset":{"version":"2.0"},)"
        R"("buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,)" +
        b64 + R"("}],)"
        R"("bufferViews":[)"
        R"({"buffer":0,"byteOffset":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":6}],)"
        R"("accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0.0,0.0,0.0],"max":[1.0,1.0,0.0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";

    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(gltf.data()),
                                           gltf.size());
    auto data = GltfLoader::load_mesh_data_from_memory(bytes, ".");
    if (!data) return std::unexpected(data.error());

    if (data->vertices.size() != 3 || data->indices.size() != 3) {
        return fail("gltf self-test: expected 3 vertices and 3 indices");
    }
    if (data->indices[0] != 0 || data->indices[1] != 1 || data->indices[2] != 2) {
        return fail("gltf self-test: unexpected index values");
    }
    if (data->bounds.min != glm::vec3(0.0F) || data->bounds.max != glm::vec3(1.0F, 1.0F, 0.0F)) {
        return fail("gltf self-test: bounds do not match the triangle extents");
    }
    // POSITION-only primitive: normal/uv/tangent should fall back to defaults.
    if (data->vertices[1].position != glm::vec3(1.0F, 0.0F, 0.0F) ||
        data->vertices[0].normal != glm::vec3(0.0F, 0.0F, 1.0F)) {
        return fail("gltf self-test: vertex attribute defaults are wrong");
    }

    auto mesh = upload_mesh(*data, allocator, transfer);
    if (!mesh) return std::unexpected(mesh.error());
    if (!mesh->valid() || mesh->vertex_buffer.handle() == VK_NULL_HANDLE) {
        return fail("gltf self-test: uploaded MeshAsset is not valid");
    }
    return {};
}

} // namespace engine::scene
