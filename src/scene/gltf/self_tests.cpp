// Legacy glTF self-tests (ROADMAP S3 step 2 moves them into tests/gltf/).

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

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "scene/gltf/internal.hpp"

// Declarations only — the stb_image implementation lives in stb_image_impl.cpp,
// compiled permissively (see CMakeLists.txt), so its warnings stay out of our
// strict TUs.
#include <stb_image.h>

namespace engine::scene {

using namespace gltf_detail;

using namespace gltf_detail;

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
        R"("baseColorTexture":{"index":0,)"
        R"("extensions":{"KHR_texture_transform":{"offset":[0.5,0.25],"scale":[2.0,4.0]}}}},)"
        R"("emissiveFactor":[0.5,0.25,0.0],"alphaCutoff":0.25,)"
        R"("extensions":{)"
        R"("KHR_materials_emissive_strength":{"emissiveStrength":2.0},)"
        R"("KHR_materials_clearcoat":{"clearcoatFactor":0.8,"clearcoatRoughnessFactor":0.3}}}]})";
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
    // emissiveFactor premultiplied by KHR_materials_emissive_strength (2.0).
    if (m.params.emissive_factor != glm::vec4(1.0F, 0.5F, 0.0F, 0.0F)) {
        return fail("material extract self-test: emissiveFactor mismatch");
    }
    if (m.base_color_image != 0 || m.normal_image != no_texture ||
        m.emissive_image != no_texture) {
        return fail("material extract self-test: texture image indices wrong");
    }
    if (m.params.flags != asset::material_has_base_color) {
        return fail("material extract self-test: only HAS_BASE_COLOR should be set");
    }
    // KHR_texture_transform on the base colour slot: scale (2,4), offset
    // (0.5,0.25), no rotation -> diagonal matrix; other slots stay identity.
    if (m.params.uv_mat[asset::material_slot_base_color] != glm::vec4(2.0F, 0.0F, 0.0F, 4.0F) ||
        m.params.uv_offset[0] != glm::vec4(0.5F, 0.25F, 0.0F, 0.0F) ||
        m.params.uv_mat[asset::material_slot_normal] != glm::vec4(1.0F, 0.0F, 0.0F, 1.0F)) {
        return fail("material extract self-test: KHR_texture_transform mismatch");
    }
    if (m.params.clearcoat_factor != 0.8F || m.params.clearcoat_roughness != 0.3F) {
        return fail("material extract self-test: clearcoat factors mismatch");
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
    fastgltf::Parser parser(kParserExtensions);
    fastgltf::Expected<fastgltf::Asset> parsed = parser.loadGltf(data.get(), ".", kLoadOptions);
    if (parsed.error() != fastgltf::Error::None) {
        return fail(std::string("gltf scene self-test: parse failed: ") +
                    std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    if (auto valid = validate_asset(parsed.get()); !valid) return std::unexpected(valid.error());

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

std::expected<void, core::Error> run_skeleton_load_self_test() {
    // Two inverse bind matrices (column-major), one per joint: identity for the
    // root, translate(-2,0,0) for the child (= inverse of its bind world).
    const std::array<float, 32> ibm{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0,  0, 0, 1,  // root: identity
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, 0, 0, 1}; // child: translate(-2,0,0)
    std::array<std::byte, sizeof(ibm)> buffer{};
    std::memcpy(buffer.data(), ibm.data(), sizeof(ibm));
    const std::string b64 = base64_encode(buffer);

    // Root (origin) with a child translated +2 X; skin joins both, IBM accessor 0.
    const std::string gltf =
        R"({"asset":{"version":"2.0"},"scene":0,)"
        R"("scenes":[{"nodes":[0]}],)"
        R"("nodes":[)"
        R"({"name":"root","children":[1]},)"
        R"({"name":"child","translation":[2.0,0.0,0.0]}],)"
        R"("skins":[{"joints":[0,1],"inverseBindMatrices":0}],)"
        R"("buffers":[{"byteLength":128,"uri":"data:application/octet-stream;base64,)" +
        b64 + R"("}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":128}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"MAT4"}]})";

    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(gltf.data()),
                                           gltf.size());
    auto skeletons = GltfLoader::load_skeletons_from_memory(bytes, ".");
    if (!skeletons) return std::unexpected(skeletons.error());
    if (skeletons->size() != 1) {
        return fail("skeleton self-test: expected exactly one skin");
    }
    const animation::SkeletonAsset& s = (*skeletons)[0];
    if (s.joints.size() != 2) {
        return fail("skeleton self-test: expected two joints");
    }
    if (s.joints[0].name != "root" || s.joints[0].parent != -1) {
        return fail("skeleton self-test: root joint name/parent wrong");
    }
    if (s.joints[1].name != "child" || s.joints[1].parent != 0) {
        return fail("skeleton self-test: child joint name/parent wrong");
    }
    if (glm::distance(s.joints[1].translation, glm::vec3(2.0F, 0.0F, 0.0F)) > 1e-5F) {
        return fail("skeleton self-test: child bind translation wrong");
    }
    if (glm::distance(glm::vec3(s.joints[1].inverse_bind[3]), glm::vec3(-2.0F, 0.0F, 0.0F)) >
        1e-5F) {
        return fail("skeleton self-test: child inverse-bind matrix wrong");
    }

    // The loaded skeleton round-trips: bind world * inverse bind == identity.
    const std::vector<glm::mat4> world = animation::compute_bind_world(s);
    for (std::size_t i = 0; i < s.joints.size(); ++i) {
        const glm::mat4 skinning = world[i] * s.joints[i].inverse_bind;
        if (glm::distance(skinning[3], glm::vec4(0.0F, 0.0F, 0.0F, 1.0F)) > 1e-5F) {
            return fail("skeleton self-test: loaded bind-pose skinning matrix is not identity");
        }
    }
    return {};
}

std::expected<void, core::Error> run_animation_load_self_test() {
    // One node animated over [0,1]: translation (LINEAR) (0,0,0)->(10,0,0),
    // scale (STEP) (1,1,1)->(2,2,2), rotation (LINEAR) identity->90 deg about Y.
    const std::array<float, 22> data{
        0.0F, 1.0F,                                     // input times       (acc0)
        0.0F, 0.0F, 0.0F, 10.0F, 0.0F, 0.0F,            // translation VEC3   (acc1)
        1.0F, 1.0F, 1.0F, 2.0F, 2.0F, 2.0F,             // scale VEC3         (acc2)
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.70710678F, 0.0F, 0.70710678F}; // rotation VEC4 (acc3)
    std::array<std::byte, sizeof(data)> buffer{};
    std::memcpy(buffer.data(), data.data(), sizeof(data));
    const std::string b64 = base64_encode(buffer);

    const std::string gltf =
        R"({"asset":{"version":"2.0"},"scene":0,)"
        R"("scenes":[{"nodes":[0]}],"nodes":[{"name":"bone"}],)"
        R"("animations":[{"name":"clip0",)"
        R"("samplers":[)"
        R"({"input":0,"output":1,"interpolation":"LINEAR"},)"
        R"({"input":0,"output":2,"interpolation":"STEP"},)"
        R"({"input":0,"output":3,"interpolation":"LINEAR"}],)"
        R"("channels":[)"
        R"({"sampler":0,"target":{"node":0,"path":"translation"}},)"
        R"({"sampler":1,"target":{"node":0,"path":"scale"}},)"
        R"({"sampler":2,"target":{"node":0,"path":"rotation"}}]}],)"
        R"("buffers":[{"byteLength":88,"uri":"data:application/octet-stream;base64,)" +
        b64 + R"("}],)"
        R"("bufferViews":[)"
        R"({"buffer":0,"byteOffset":0,"byteLength":8},)"
        R"({"buffer":0,"byteOffset":8,"byteLength":24},)"
        R"({"buffer":0,"byteOffset":32,"byteLength":24},)"
        R"({"buffer":0,"byteOffset":56,"byteLength":32}],)"
        R"("accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0.0],"max":[1.0]},)"
        R"({"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},)"
        R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"},)"
        R"({"bufferView":3,"componentType":5126,"count":2,"type":"VEC4"}]})";

    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(gltf.data()),
                                           gltf.size());
    auto clips = GltfLoader::load_animations_from_memory(bytes, ".");
    if (!clips) return std::unexpected(clips.error());
    if (clips->size() != 1) {
        return fail("animation self-test: expected exactly one clip");
    }
    const animation::AnimClipAsset& clip = (*clips)[0];
    if (clip.name != "clip0" || std::abs(clip.duration - 1.0F) > 1e-5F) {
        return fail("animation self-test: clip name/duration wrong");
    }
    if (clip.channels.size() != 3 || clip.samplers.size() != 3) {
        return fail("animation self-test: wrong channel/sampler count");
    }

    const auto sampler_for = [&](animation::AnimPath p) -> const animation::AnimSampler* {
        for (const animation::AnimChannel& c : clip.channels) {
            if (c.path == p && c.target_node == 0 && c.sampler < clip.samplers.size()) {
                return &clip.samplers[c.sampler];
            }
        }
        return nullptr;
    };

    const animation::AnimSampler* ts = sampler_for(animation::AnimPath::translation);
    const animation::AnimSampler* ss = sampler_for(animation::AnimPath::scale);
    const animation::AnimSampler* rs = sampler_for(animation::AnimPath::rotation);
    if (ts == nullptr || ss == nullptr || rs == nullptr) {
        return fail("animation self-test: a channel/path is missing");
    }
    if (glm::distance(ts->sample_vec3(0.5F), glm::vec3(5, 0, 0)) > 1e-5F) {
        return fail("animation self-test: translation sample wrong");
    }
    if (glm::distance(ss->sample_vec3(0.5F), glm::vec3(1, 1, 1)) > 1e-5F) {
        return fail("animation self-test: STEP scale sample wrong");
    }
    const glm::quat r = rs->sample_quat(0.5F);
    const glm::quat expected(0.92387953F, 0.0F, 0.38268343F, 0.0F);
    if (glm::abs(glm::dot(r, expected)) < 0.9999F) {
        return fail("animation self-test: rotation slerp sample wrong");
    }
    return {};
}

std::expected<void, core::Error> run_gltf_validation_self_test() {
    // Shared buffer: 3 float3 positions (36 bytes) + 3 float2 UVs (24 bytes),
    // all zero — only structure matters here.
    const std::string b64(80, 'A'); // 60 bytes of zeros
    const auto doc = [&](const std::string& body) {
        return std::string(R"({"asset":{"version":"2.0"},)") + body +
               R"("buffers":[{"byteLength":60,"uri":"data:application/octet-stream;base64,)" + b64 +
               R"("}],)"
               R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
               R"({"buffer":0,"byteOffset":36,"byteLength":24}],)"
               R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
               R"("min":[0,0,0],"max":[0,0,0]},)"
               R"({"bufferView":1,"componentType":5126,"count":2,"type":"VEC2"}]})";
    };
    const auto parses = [](const std::string& gltf) -> std::expected<void, core::Error> {
        auto data = fastgltf::GltfDataBuffer::FromBytes(
            reinterpret_cast<const std::byte*>(gltf.data()), gltf.size());
        if (data.error() != fastgltf::Error::None) return fail("could not wrap buffer");
        fastgltf::Parser parser(kParserExtensions);
        fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data.get(), ".", kLoadOptions);
        if (asset.error() != fastgltf::Error::None) {
            return fail(std::string(fastgltf::getErrorMessage(asset.error())));
        }
        return validate_asset(asset.get());
    };
    const std::string mesh = R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)";

    if (auto ok = parses(doc(mesh + R"("nodes":[{"mesh":0,"children":[1]},{"mesh":0}],)")); !ok) {
        return fail("gltf validation self-test: well-formed document rejected: " + ok.error().message);
    }
    const struct {
        const char* what;
        std::string body;
    } bad[] = {
        {"two-node cycle", mesh + R"("nodes":[{"children":[1]},{"children":[0]}],)"},
        {"self child", mesh + R"("nodes":[{"children":[0]}],)"},
        {"two parents", mesh + R"("nodes":[{"children":[2]},{"children":[2]},{}],)"},
        {"child out of range", mesh + R"("nodes":[{"children":[7]}],)"},
        {"mesh index out of range", mesh + R"("nodes":[{"mesh":3}],)"},
        {"material out of range",
         R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":2}]}],)"},
        {"accessor out of range",
         R"("meshes":[{"primitives":[{"attributes":{"POSITION":9}}]}],)"},
        {"attribute count mismatch",
         R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1}}]}],)"},
    };
    for (const auto& b : bad) {
        if (parses(doc(b.body))) {
            return fail(std::string("gltf validation self-test: accepted ") + b.what);
        }
    }
    return {};
}

} // namespace engine::scene
