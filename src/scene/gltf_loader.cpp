#include "engine/scene/gltf_loader.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/glm.hpp>

// Declarations only — the stb_image implementation lives in stb_image_impl.cpp,
// compiled permissively (see CMakeLists.txt), so its warnings stay out of our
// strict TUs.
#include <stb_image.h>

namespace engine::scene {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr auto kLoadOptions = fastgltf::Options::LoadExternalBuffers |
                              fastgltf::Options::LoadExternalImages |
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

// -------------------------------------------------------------------------
// Textures
// -------------------------------------------------------------------------

template <typename Container>
[[nodiscard]] std::span<const std::byte> as_byte_span(const Container& c) {
    return {c.data(), c.size()};
}

// Resolve the encoded image/buffer bytes behind a fastgltf DataSource. After
// LoadExternalBuffers/LoadExternalImages, images resolve to sources::Array or a
// sources::BufferView into an already-loaded buffer.
[[nodiscard]] std::expected<std::span<const std::byte>, core::Error>
data_source_bytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source) {
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& a)
                -> std::expected<std::span<const std::byte>, core::Error> {
                return as_byte_span(a.bytes);
            },
            [](const fastgltf::sources::Vector& v)
                -> std::expected<std::span<const std::byte>, core::Error> {
                return as_byte_span(v.bytes);
            },
            [](const fastgltf::sources::ByteView& b)
                -> std::expected<std::span<const std::byte>, core::Error> {
                return b.bytes;
            },
            [&](const fastgltf::sources::BufferView& bv)
                -> std::expected<std::span<const std::byte>, core::Error> {
                const fastgltf::BufferView& view = asset.bufferViews[bv.bufferViewIndex];
                auto buffer = data_source_bytes(asset, asset.buffers[view.bufferIndex].data);
                if (!buffer) return buffer;
                if (view.byteOffset + view.byteLength > buffer->size()) {
                    return fail("glTF image bufferView out of range");
                }
                return buffer->subspan(view.byteOffset, view.byteLength);
            },
            [](const auto&) -> std::expected<std::span<const std::byte>, core::Error> {
                return fail("glTF image has an unsupported / unloaded data source "
                            "(external file not resolved?)");
            }},
        source);
}

// Per-image colour space: sRGB if any material references the image as a base
// colour or emissive texture, otherwise linear (normal, metallic-roughness, AO).
[[nodiscard]] std::vector<asset::TextureColorSpace>
resolve_color_spaces(const fastgltf::Asset& asset) {
    std::vector<asset::TextureColorSpace> spaces(asset.images.size(),
                                                 asset::TextureColorSpace::linear);
    const auto mark_srgb = [&](const fastgltf::Optional<fastgltf::TextureInfo>& info) {
        if (!info.has_value()) return;
        const fastgltf::Texture& tex = asset.textures[info->textureIndex];
        if (tex.imageIndex.has_value()) {
            spaces[tex.imageIndex.value()] = asset::TextureColorSpace::srgb;
        }
    };
    for (const fastgltf::Material& mat : asset.materials) {
        mark_srgb(mat.pbrData.baseColorTexture);
        mark_srgb(mat.emissiveTexture);
    }
    return spaces;
}

[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset) {
    const std::vector<asset::TextureColorSpace> spaces = resolve_color_spaces(asset);
    std::vector<TextureData> out;
    out.reserve(asset.images.size());
    for (std::size_t i = 0; i < asset.images.size(); ++i) {
        auto encoded = data_source_bytes(asset, asset.images[i].data);
        if (!encoded) return std::unexpected(encoded.error());
        auto decoded = decode_image(*encoded, spaces[i]);
        if (!decoded) return std::unexpected(decoded.error());
        out.push_back(std::move(*decoded));
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
parse_and_extract_textures(fastgltf::GltfDataBuffer& data,
                           const std::filesystem::path& base_dir) {
    fastgltf::Parser parser;
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data, base_dir, kLoadOptions);
    if (asset.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(asset.error())));
    }
    return extract_textures(asset.get());
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
// Textures
// ---------------------------------------------------------------------------
std::expected<TextureData, core::Error>
decode_image(std::span<const std::byte> encoded, asset::TextureColorSpace color_space) {
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(encoded.data()),
        static_cast<int>(encoded.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        return fail("stb_image: failed to decode image");
    }

    TextureData out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.color_space = color_space;
    const std::size_t byte_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U;
    out.pixels.resize(byte_count);
    std::memcpy(out.pixels.data(), pixels, byte_count);
    stbi_image_free(pixels);
    return out;
}

std::expected<asset::TextureAsset, core::Error>
upload_texture(const TextureData& data, rhi::GpuAllocator& allocator,
               const rhi::TransferContext& transfer) {
    if (data.width == 0 || data.height == 0 || data.pixels.empty()) {
        return fail("upload_texture: empty texture data");
    }
    const VkFormat format = data.color_space == asset::TextureColorSpace::srgb
                                ? VK_FORMAT_R8G8B8A8_SRGB
                                : VK_FORMAT_R8G8B8A8_UNORM;
    const VkExtent2D extent{data.width, data.height};

    auto image = rhi::upload_device_image(allocator, transfer, data.pixels.data(),
                                          data.pixels.size(), extent, format);
    if (!image) return std::unexpected(image.error());

    auto view = rhi::create_image_view(transfer.device, image->handle(), format);
    if (!view) return std::unexpected(view.error());

    auto sampler = rhi::create_sampler(transfer.device);
    if (!sampler) return std::unexpected(sampler.error());

    asset::TextureAsset texture;
    texture.image = std::move(*image);
    texture.view = std::move(*view);
    texture.sampler = std::move(*sampler);
    texture.width = data.width;
    texture.height = data.height;
    texture.format = format;
    return texture;
}

std::expected<std::vector<TextureData>, core::Error>
GltfLoader::load_texture_data(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_textures(data.get(), path.parent_path());
}

std::expected<std::vector<TextureData>, core::Error>
GltfLoader::load_texture_data_from_memory(std::span<const std::byte> bytes,
                                          const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_textures(data.get(), base_dir);
}

std::expected<std::vector<asset::TextureAsset>, core::Error>
GltfLoader::load_textures(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                          const rhi::TransferContext& transfer) {
    auto data = load_texture_data(path);
    if (!data) return std::unexpected(data.error());

    std::vector<asset::TextureAsset> textures;
    textures.reserve(data->size());
    for (const TextureData& td : *data) {
        auto texture = upload_texture(td, allocator, transfer);
        if (!texture) return std::unexpected(texture.error());
        textures.push_back(std::move(*texture));
    }
    return textures;
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

std::expected<void, core::Error>
run_texture_loader_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    // A 2x2 RGBA8 PNG: top-left red, top-right green, bottom-left blue,
    // bottom-right half-alpha yellow (generated offline, verified below).
    static constexpr std::array<std::uint8_t, 77> png_bytes{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49,
        0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06,
        0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44,
        0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x81, 0x34,
        0x10, 0x30, 0x34, 0x00, 0x00, 0x47, 0x4b, 0x08, 0x79, 0xc3, 0x25, 0x87, 0xeb,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    const std::span<const std::byte> png(reinterpret_cast<const std::byte*>(png_bytes.data()),
                                         png_bytes.size());

    // Direct decode path: verify dimensions and a known pixel (top-left = red).
    auto decoded = decode_image(png, asset::TextureColorSpace::srgb);
    if (!decoded) return std::unexpected(decoded.error());
    if (decoded->width != 2 || decoded->height != 2 || decoded->pixels.size() != 16) {
        return fail("texture self-test: decoded 2x2 PNG has wrong dimensions");
    }
    const auto px = [&](std::size_t i) {
        return std::to_integer<std::uint8_t>(decoded->pixels[i]);
    };
    if (px(0) != 255 || px(1) != 0 || px(2) != 0 || px(3) != 255) {
        return fail("texture self-test: top-left pixel is not opaque red");
    }

    // glTF path: embed the PNG as a baseColorTexture data URI and verify the
    // loader tags it sRGB and decodes one image.
    const std::string b64 = base64_encode(png);
    const std::string gltf =
        R"({"asset":{"version":"2.0"},)"
        R"("images":[{"mimeType":"image/png","uri":"data:image/png;base64,)" +
        b64 + R"("}],)"
        R"("samplers":[{}],)"
        R"("textures":[{"source":0,"sampler":0}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}]})";
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(gltf.data()),
                                           gltf.size());

    auto tex_data = GltfLoader::load_texture_data_from_memory(bytes, ".");
    if (!tex_data) return std::unexpected(tex_data.error());
    if (tex_data->size() != 1) {
        return fail("texture self-test: expected exactly one decoded image");
    }
    if ((*tex_data)[0].color_space != asset::TextureColorSpace::srgb ||
        (*tex_data)[0].width != 2 || (*tex_data)[0].height != 2) {
        return fail("texture self-test: baseColorTexture not tagged sRGB / wrong size");
    }

    auto texture = upload_texture((*tex_data)[0], allocator, transfer);
    if (!texture) return std::unexpected(texture.error());
    if (!texture->valid() || texture->format != VK_FORMAT_R8G8B8A8_SRGB) {
        return fail("texture self-test: uploaded TextureAsset is not valid");
    }
    return {};
}

} // namespace engine::scene
