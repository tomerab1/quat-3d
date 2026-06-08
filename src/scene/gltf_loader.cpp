#include "engine/scene/gltf_loader.hpp"

#include <array>
#include <cstring>
#include <functional>
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
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

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

    if (!prim.indicesAccessor.has_value()) {
        return fail("glTF primitive has no index accessor after generation");
    }
    const fastgltf::Accessor& idx_acc = asset.accessors[prim.indicesAccessor.value()];
    out.indices.reserve(idx_acc.count);
    fastgltf::iterateAccessor<std::uint32_t>(
        asset, idx_acc, [&](std::uint32_t idx) { out.indices.push_back(idx); });

    if (out.vertices.empty() || out.indices.empty()) {
        return fail("glTF primitive contained no triangle geometry");
    }
    out.bounds = {bmin, bmax};
    out.submeshes = {asset::SubMesh{0, static_cast<std::uint32_t>(out.indices.size()), 0}};
    return out;
}

// glTF node local transform as a glm matrix (column-major). A node carries either
// an explicit matrix or a TRS triple; both resolve to the same composition.
[[nodiscard]] glm::mat4 node_local_matrix(const fastgltf::Node& node) {
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::TRS& trs) {
                const glm::vec3 t = to_glm(trs.translation);
                // fastgltf quaternion is [x, y, z, w]; glm::quat ctor is (w,x,y,z).
                const glm::quat q(trs.rotation[3], trs.rotation[0], trs.rotation[1],
                                  trs.rotation[2]);
                const glm::vec3 s = to_glm(trs.scale);
                return glm::translate(glm::mat4(1.0F), t) * glm::mat4_cast(q) *
                       glm::scale(glm::mat4(1.0F), s);
            },
            [](const fastgltf::math::fmat4x4& m) {
                glm::mat4 out(1.0F);
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        out[c][r] = m[c][r];
                    }
                }
                return out;
            }},
        node.transform);
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

// -------------------------------------------------------------------------
// Materials
// -------------------------------------------------------------------------

// glTF image index behind a texture reference, or no_texture when unset. A
// texture with no image source also resolves to no_texture. Templated on the
// whole optional type: fastgltf::Optional is an alias (conditional_t), so the
// contained TextureInfo type can't be deduced through it directly.
template <typename OptTextureInfo>
[[nodiscard]] std::size_t image_of(const fastgltf::Asset& asset,
                                   const OptTextureInfo& info) {
    if (!info.has_value()) return no_texture;
    const fastgltf::Texture& tex = asset.textures[info->textureIndex];
    return tex.imageIndex.has_value() ? tex.imageIndex.value() : no_texture;
}

[[nodiscard]] std::vector<MaterialData> extract_materials(const fastgltf::Asset& asset) {
    std::vector<MaterialData> out;
    out.reserve(asset.materials.size());
    for (const fastgltf::Material& mat : asset.materials) {
        MaterialData md;
        asset::PbrMaterialParams& p = md.params;

        const auto& bcf = mat.pbrData.baseColorFactor;
        p.base_color_factor = {bcf[0], bcf[1], bcf[2], bcf[3]};
        const auto& ef = mat.emissiveFactor;
        p.emissive_factor = {ef[0], ef[1], ef[2], 0.0F};
        p.metallic_factor = mat.pbrData.metallicFactor;
        p.roughness_factor = mat.pbrData.roughnessFactor;
        p.alpha_cutoff = mat.alphaCutoff;
        if (mat.normalTexture.has_value()) p.normal_scale = mat.normalTexture->scale;
        if (mat.occlusionTexture.has_value()) {
            p.occlusion_strength = mat.occlusionTexture->strength;
        }

        md.base_color_image = image_of(asset, mat.pbrData.baseColorTexture);
        md.metallic_roughness_image = image_of(asset, mat.pbrData.metallicRoughnessTexture);
        md.normal_image = image_of(asset, mat.normalTexture);
        md.occlusion_image = image_of(asset, mat.occlusionTexture);
        md.emissive_image = image_of(asset, mat.emissiveTexture);

        std::uint32_t flags = 0;
        if (md.base_color_image != no_texture) flags |= asset::material_has_base_color;
        if (md.normal_image != no_texture) flags |= asset::material_has_normal;
        if (md.metallic_roughness_image != no_texture) {
            flags |= asset::material_has_metallic_roughness;
        }
        if (md.emissive_image != no_texture) flags |= asset::material_has_emissive;
        if (md.occlusion_image != no_texture) flags |= asset::material_has_occlusion;
        p.flags = flags;

        out.push_back(md);
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<MaterialData>, core::Error>
parse_and_extract_materials(fastgltf::GltfDataBuffer& data,
                            const std::filesystem::path& base_dir) {
    fastgltf::Parser parser;
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data, base_dir, kLoadOptions);
    if (asset.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(asset.error())));
    }
    return extract_materials(asset.get());
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

std::expected<std::vector<MaterialData>, core::Error>
GltfLoader::load_material_data(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_materials(data.get(), path.parent_path());
}

std::expected<std::vector<MaterialData>, core::Error>
GltfLoader::load_material_data_from_memory(std::span<const std::byte> bytes,
                                           const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_materials(data.get(), base_dir);
}

// ---------------------------------------------------------------------------
// Scene instantiation
// ---------------------------------------------------------------------------

// Build ECS entities from a parsed glTF asset. File-local so both the path-based
// public entry and the in-memory self-test share it. `key_prefix` namespaces the
// AssetManager cache keys (so two files' meshes/materials don't collide).
[[nodiscard]] static std::expected<std::vector<entt::entity>, core::Error>
assemble_scene(const fastgltf::Asset& a, const std::string& key_prefix,
               rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
               asset::AssetManager& assets, Scene& scene) {
    // Textures: decode + upload each image once, cached by "<path>#img<i>".
    auto tex_datas = extract_textures(a);
    if (!tex_datas) return std::unexpected(tex_datas.error());
    std::vector<asset::AssetHandle<asset::TextureAsset>> tex_handles(a.images.size());
    for (std::size_t i = 0; i < a.images.size(); ++i) {
        const std::string k = key_prefix + "#img" + std::to_string(i);
        tex_handles[i] = assets.load<asset::TextureAsset>(
            k, [&](const std::filesystem::path&) {
                return upload_texture((*tex_datas)[i], allocator, transfer);
            });
    }

    // Materials: wire the decoded textures into each material's slots, upload.
    const std::vector<MaterialData> mat_datas = extract_materials(a);
    const auto tex_or_null = [&](std::size_t image) {
        return image != no_texture ? tex_handles[image]
                                   : asset::AssetHandle<asset::TextureAsset>{};
    };
    std::vector<asset::AssetHandle<asset::MaterialAsset>> mat_handles(a.materials.size());
    for (std::size_t i = 0; i < a.materials.size(); ++i) {
        const MaterialData& md = mat_datas[i];
        asset::MaterialTextures textures;
        textures.base_color = tex_or_null(md.base_color_image);
        textures.normal = tex_or_null(md.normal_image);
        textures.metallic_roughness = tex_or_null(md.metallic_roughness_image);
        textures.emissive = tex_or_null(md.emissive_image);
        textures.occlusion = tex_or_null(md.occlusion_image);
        const std::string k = key_prefix + "#mat" + std::to_string(i);
        mat_handles[i] = assets.load<asset::MaterialAsset>(
            k, [&](const std::filesystem::path&) {
                return asset::upload_material(md.params, textures, allocator, transfer);
            });
    }

    // Mesh primitives: one MeshAsset (single submesh) + material handle each.
    struct PrimRef {
        asset::AssetHandle<asset::MeshAsset>     mesh;
        asset::AssetHandle<asset::MaterialAsset> material;
    };
    std::vector<std::vector<PrimRef>> mesh_prims(a.meshes.size());
    for (std::size_t m = 0; m < a.meshes.size(); ++m) {
        const fastgltf::Mesh& mesh = a.meshes[m];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const fastgltf::Primitive& prim = mesh.primitives[p];
            if (prim.type != fastgltf::PrimitiveType::Triangles) continue;
            auto md = extract_primitive(a, prim);
            if (!md) return std::unexpected(md.error());
            const MeshData prim_data = std::move(*md);
            const std::string k =
                key_prefix + "#mesh" + std::to_string(m) + ".prim" + std::to_string(p);
            PrimRef ref;
            ref.mesh = assets.load<asset::MeshAsset>(
                k, [&](const std::filesystem::path&) {
                    return upload_mesh(prim_data, allocator, transfer);
                });
            ref.material = prim.materialIndex.has_value()
                               ? mat_handles[prim.materialIndex.value()]
                               : asset::AssetHandle<asset::MaterialAsset>{};
            mesh_prims[m].push_back(ref);
        }
    }

    // Walk the node hierarchy into ECS entities. Node TRS -> Transform.local; a
    // single-primitive mesh becomes a MeshRenderer on the node, a multi-primitive
    // mesh fans out into one child entity per primitive.
    std::vector<entt::entity> roots;
    entt::registry& reg = scene.registry();
    std::function<void(std::size_t, entt::entity)> build =
        [&](std::size_t node_index, entt::entity parent) {
            const fastgltf::Node& n = a.nodes[node_index];
            std::string name(n.name.begin(), n.name.end());
            const entt::entity e = scene.create_entity(std::move(name));
            reg.get<Transform>(e).local = node_local_matrix(n);
            if (parent != entt::null) {
                scene.set_parent(e, parent);
            } else {
                roots.push_back(e);
            }

            if (n.meshIndex.has_value()) {
                const std::vector<PrimRef>& prims = mesh_prims[n.meshIndex.value()];
                if (prims.size() == 1) {
                    reg.emplace<MeshRenderer>(e, prims[0].mesh, prims[0].material);
                } else {
                    for (std::size_t p = 0; p < prims.size(); ++p) {
                        const entt::entity pe = scene.create_entity(
                            std::string(n.name.begin(), n.name.end()) + ".prim" +
                            std::to_string(p));
                        scene.set_parent(pe, e);
                        reg.emplace<MeshRenderer>(pe, prims[p].mesh, prims[p].material);
                    }
                }
            }
            for (std::size_t child : n.children) {
                build(child, e);
            }
        };

    const std::size_t scene_index = a.defaultScene.value_or(0);
    if (scene_index < a.scenes.size()) {
        for (std::size_t root : a.scenes[scene_index].nodeIndices) {
            build(root, entt::null);
        }
    } else {
        // No scene block: roots are nodes not referenced as anyone's child.
        std::vector<bool> is_child(a.nodes.size(), false);
        for (const fastgltf::Node& n : a.nodes) {
            for (std::size_t c : n.children) is_child[c] = true;
        }
        for (std::size_t i = 0; i < a.nodes.size(); ++i) {
            if (!is_child[i]) build(i, entt::null);
        }
    }
    return roots;
}

std::expected<std::vector<entt::entity>, core::Error>
GltfLoader::instantiate(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                        const rhi::TransferContext& transfer, asset::AssetManager& assets,
                        Scene& scene) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    fastgltf::Parser parser;
    fastgltf::Expected<fastgltf::Asset> parsed =
        parser.loadGltf(data.get(), path.parent_path(), kLoadOptions);
    if (parsed.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    return assemble_scene(parsed.get(), path.generic_string(), allocator, transfer, assets, scene);
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

std::expected<void, core::Error> run_material_extract_self_test() {
    // A material with known factors and a baseColorTexture (image 0) but no other
    // textures — so exactly one HAS_* flag should be set.
    const std::string gltf =
        R"({"asset":{"version":"2.0"},)"
        R"("images":[{"uri":"data:image/png;base64,iVBORw0KGgo="}],)"
        R"("samplers":[{}],)"
        R"("textures":[{"source":0,"sampler":0}],)"
        R"("materials":[{)"
        R"("pbrMetallicRoughness":{)"
        R"("baseColorFactor":[0.2,0.4,0.6,1.0],)"
        R"("metallicFactor":0.1,"roughnessFactor":0.7,)"
        R"("baseColorTexture":{"index":0}},)"
        R"("emissiveFactor":[0.5,0.25,0.0],"alphaCutoff":0.25}]})";
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(gltf.data()),
                                           gltf.size());

    auto mats = GltfLoader::load_material_data_from_memory(bytes, ".");
    if (!mats) return std::unexpected(mats.error());
    if (mats->size() != 1) {
        return fail("material extract self-test: expected exactly one material");
    }
    const MaterialData& m = (*mats)[0];
    if (m.params.base_color_factor != glm::vec4(0.2F, 0.4F, 0.6F, 1.0F)) {
        return fail("material extract self-test: baseColorFactor mismatch");
    }
    if (m.params.metallic_factor != 0.1F || m.params.roughness_factor != 0.7F ||
        m.params.alpha_cutoff != 0.25F) {
        return fail("material extract self-test: scalar factors mismatch");
    }
    if (m.params.emissive_factor != glm::vec4(0.5F, 0.25F, 0.0F, 0.0F)) {
        return fail("material extract self-test: emissiveFactor mismatch");
    }
    if (m.base_color_image != 0 || m.normal_image != no_texture ||
        m.emissive_image != no_texture) {
        return fail("material extract self-test: texture image indices wrong");
    }
    if (m.params.flags != asset::material_has_base_color) {
        return fail("material extract self-test: only HAS_BASE_COLOR should be set");
    }
    return {};
}

std::expected<void, core::Error>
run_gltf_scene_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    // Triangle geometry (positions then uint16 indices), as in the mesh test.
    const std::array<float, 9> positions{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    const std::array<std::uint16_t, 3> indices{0, 1, 2};
    std::array<std::byte, 42> buffer{};
    std::memcpy(buffer.data(), positions.data(), sizeof(positions));
    std::memcpy(buffer.data() + sizeof(positions), indices.data(), sizeof(indices));
    const std::string b64 = base64_encode(buffer);

    // A two-node hierarchy: "parent" (translated +2 X, mesh 0) with child "child"
    // (translated +3 Y, mesh 0). One scene rooted at the parent.
    const std::string gltf =
        R"({"asset":{"version":"2.0"},"scene":0,)"
        R"("scenes":[{"nodes":[0]}],)"
        R"("nodes":[)"
        R"({"name":"parent","translation":[2.0,0.0,0.0],"mesh":0,"children":[1]},)"
        R"({"name":"child","translation":[0.0,3.0,0.0],"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,1.0],)"
        R"("metallicFactor":0.0}}],)"
        R"("buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,)" +
        b64 + R"("}],)"
        R"("bufferViews":[)"
        R"({"buffer":0,"byteOffset":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":6}],)"
        R"("accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0.0,0.0,0.0],"max":[1.0,1.0,0.0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";

    auto data = fastgltf::GltfDataBuffer::FromBytes(
        reinterpret_cast<const std::byte*>(gltf.data()), gltf.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("gltf scene self-test: could not wrap buffer");
    }
    fastgltf::Parser parser;
    fastgltf::Expected<fastgltf::Asset> parsed = parser.loadGltf(data.get(), ".", kLoadOptions);
    if (parsed.error() != fastgltf::Error::None) {
        return fail(std::string("gltf scene self-test: parse failed: ") +
                    std::string(fastgltf::getErrorMessage(parsed.error())));
    }

    Scene scene;
    asset::AssetManager assets;
    auto roots = assemble_scene(parsed.get(), "scene_selftest", allocator, transfer, assets, scene);
    if (!roots) return std::unexpected(roots.error());

    if (roots->size() != 1) {
        return fail("gltf scene self-test: expected exactly one root node");
    }
    entt::registry& reg = scene.registry();
    if (reg.view<Transform>().size() != 2) {
        return fail("gltf scene self-test: expected two node entities");
    }
    if (reg.view<MeshRenderer>().size() != 2) {
        return fail("gltf scene self-test: both nodes should carry a MeshRenderer");
    }

    const entt::entity parent = roots->front();
    const auto* kids = reg.try_get<Children>(parent);
    if (kids == nullptr || kids->entities.size() != 1) {
        return fail("gltf scene self-test: parent should have exactly one child");
    }
    const entt::entity child = kids->entities.front();
    if (reg.get<Parent>(child).entity != parent) {
        return fail("gltf scene self-test: child->parent link is wrong");
    }

    // Local translations come straight from the node TRS.
    if (glm::distance(glm::vec3(reg.get<Transform>(parent).local[3]),
                      glm::vec3(2.0F, 0.0F, 0.0F)) > 1e-4F) {
        return fail("gltf scene self-test: parent local translation wrong");
    }
    if (glm::distance(glm::vec3(reg.get<Transform>(child).local[3]),
                      glm::vec3(0.0F, 3.0F, 0.0F)) > 1e-4F) {
        return fail("gltf scene self-test: child local translation wrong");
    }

    // After a tick, the child's world transform composes through the parent.
    scene.tick();
    if (glm::distance(glm::vec3(reg.get<Transform>(child).world[3]),
                      glm::vec3(2.0F, 3.0F, 0.0F)) > 1e-4F) {
        return fail("gltf scene self-test: child world transform did not compose");
    }

    // The mesh handle resolved and uploaded.
    if (!reg.get<MeshRenderer>(parent).mesh.is_loaded()) {
        return fail("gltf scene self-test: parent mesh handle did not load");
    }
    return {};
}

} // namespace engine::scene
