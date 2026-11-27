// glTF structural validation (see internal.hpp).

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

namespace engine::scene {

namespace gltf_detail {

namespace {

// [offset, offset + length) lies within `size` bytes.
[[nodiscard]] bool in_range(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}

// Buffer views lie inside loaded buffers and accessors (dense and sparse)
// inside their views: fastgltf's accessor iteration reads without bounds
// checks. Runs after EXT_meshopt_compression decoding, so a view still on a
// data-less fallback buffer is rejected here.
[[nodiscard]] std::expected<void, core::Error> validate_buffer_ranges(const fastgltf::Asset& asset) {
    for (std::size_t vi = 0; vi < asset.bufferViews.size(); ++vi) {
        const fastgltf::BufferView& view = asset.bufferViews[vi];
        const std::string what = "glTF: bufferView " + std::to_string(vi);
        if (view.bufferIndex >= asset.buffers.size()) return fail(what + " buffer index out of range");
        auto bytes = data_source_bytes(asset, asset.buffers[view.bufferIndex].data);
        if (!bytes) return fail(what + ": " + bytes.error().message);
        if (!in_range(view.byteOffset, view.byteLength, bytes->size())) {
            return fail(what + " exceeds its buffer");
        }
    }
    const auto view_holds = [&](std::size_t view, std::size_t offset, std::size_t count,
                                std::size_t elem, std::size_t stride) {
        if (view >= asset.bufferViews.size()) return false;
        if (count == 0) return true;
        const std::size_t length = asset.bufferViews[view].byteLength;
        if (stride < elem || count - 1 > length / stride) return false;
        return in_range(offset, (count - 1) * stride + elem, length);
    };
    for (std::size_t ai = 0; ai < asset.accessors.size(); ++ai) {
        const fastgltf::Accessor& acc = asset.accessors[ai];
        const std::string what = "glTF: accessor " + std::to_string(ai);
        const std::size_t elem = fastgltf::getElementByteSize(acc.type, acc.componentType);
        if (elem == 0) return fail(what + " has an invalid type");
        if (acc.bufferViewIndex.has_value()) {
            const std::size_t view = acc.bufferViewIndex.value();
            const std::size_t stride = view < asset.bufferViews.size()
                                           ? asset.bufferViews[view].byteStride.value_or(elem)
                                           : elem;
            if (!view_holds(view, acc.byteOffset, acc.count, elem, stride)) {
                return fail(what + " exceeds its bufferView");
            }
        }
        if (acc.sparse.has_value()) {
            const fastgltf::SparseAccessor& sp = acc.sparse.value();
            const std::size_t index_size = fastgltf::getComponentByteSize(sp.indexComponentType);
            if (sp.count > acc.count || index_size == 0 ||
                !view_holds(sp.indicesBufferView, sp.indicesByteOffset, sp.count, index_size,
                            index_size) ||
                !view_holds(sp.valuesBufferView, sp.valuesByteOffset, sp.count, elem, elem)) {
                return fail(what + " sparse storage out of range");
            }
        }
    }
    return {};
}

} // namespace

// Structural validation of a parsed asset, run before any extraction. glTF
// files are untrusted input: every cross-reference the loader follows is
// range-checked here, attribute accessors must agree with POSITION's count
// (they are written by index into arrays sized from it), and the node graph
// must be a forest (each node has at most one parent, no cycles) because
// scene assembly, skin extraction and world-matrix resolution recurse over
// it. fastgltf::validate is deliberately NOT used: it enforces the whole
// spec (e.g. POSITION min/max) and rejects files real exporters produce and
// the loader handles fine — these checks cover exactly what the loader
// dereferences.
[[nodiscard]] std::expected<void, core::Error> validate_asset(const fastgltf::Asset& asset) {
    if (auto ok = validate_buffer_ranges(asset); !ok) return ok;
    const std::size_t n_acc = asset.accessors.size();
    const std::size_t n_nodes = asset.nodes.size();

    for (std::size_t mi = 0; mi < asset.meshes.size(); ++mi) {
        for (const fastgltf::Primitive& prim : asset.meshes[mi].primitives) {
            const auto* pos = prim.findAttribute("POSITION");
            std::size_t vertex_count = 0;
            if (pos != prim.attributes.cend()) {
                if (pos->accessorIndex >= n_acc) return fail("glTF: POSITION accessor out of range");
                vertex_count = asset.accessors[pos->accessorIndex].count;
            }
            for (const auto& attr : prim.attributes) {
                if (attr.accessorIndex >= n_acc) {
                    return fail("glTF: mesh " + std::to_string(mi) + " attribute accessor out of range");
                }
                if (pos != prim.attributes.cend() &&
                    asset.accessors[attr.accessorIndex].count != vertex_count) {
                    return fail("glTF: mesh " + std::to_string(mi) + " attribute '" +
                                std::string(std::string_view(attr.name)) + "' count differs from POSITION");
                }
            }
            if (prim.indicesAccessor.has_value() && prim.indicesAccessor.value() >= n_acc) {
                return fail("glTF: index accessor out of range");
            }
            if (prim.materialIndex.has_value() &&
                prim.materialIndex.value() >= asset.materials.size()) {
                return fail("glTF: primitive material index out of range");
            }
        }
    }

    for (const fastgltf::Texture& tex : asset.textures) {
        if ((tex.imageIndex.has_value() && tex.imageIndex.value() >= asset.images.size()) ||
            (tex.basisuImageIndex.has_value() &&
             tex.basisuImageIndex.value() >= asset.images.size())) {
            return fail("glTF: texture image index out of range");
        }
    }
    const auto check_texture = [&](const auto& info) -> bool {
        return !info.has_value() || info->textureIndex < asset.textures.size();
    };
    for (const fastgltf::Material& mat : asset.materials) {
        bool ok = check_texture(mat.pbrData.baseColorTexture) &&
                  check_texture(mat.pbrData.metallicRoughnessTexture) &&
                  check_texture(mat.normalTexture) && check_texture(mat.occlusionTexture) &&
                  check_texture(mat.emissiveTexture);
        if (mat.volume != nullptr) ok = ok && check_texture(mat.volume->thicknessTexture);
        if (!ok) return fail("glTF: material texture or image index out of range");
    }

    // Node graph: references in range, at most one parent, acyclic.
    std::vector<std::uint8_t> has_parent(n_nodes, 0);
    for (std::size_t ni = 0; ni < n_nodes; ++ni) {
        const fastgltf::Node& n = asset.nodes[ni];
        if (n.meshIndex.has_value() && n.meshIndex.value() >= asset.meshes.size()) {
            return fail("glTF: node mesh index out of range");
        }
        if (n.skinIndex.has_value() && n.skinIndex.value() >= asset.skins.size()) {
            return fail("glTF: node skin index out of range");
        }
        for (const std::size_t c : n.children) {
            if (c >= n_nodes || c == ni) return fail("glTF: node child index invalid");
            if (has_parent[c] != 0) return fail("glTF: node has more than one parent");
            has_parent[c] = 1;
        }
    }
    // With one parent per node, a cycle is exactly a set of nodes no root
    // reaches: walk down from the roots iteratively and require full coverage.
    {
        std::vector<std::uint8_t> reached(n_nodes, 0);
        std::vector<std::size_t> stack;
        for (std::size_t ni = 0; ni < n_nodes; ++ni) {
            if (has_parent[ni] == 0) stack.push_back(ni);
        }
        std::size_t count = 0;
        while (!stack.empty()) {
            const std::size_t ni = stack.back();
            stack.pop_back();
            if (reached[ni] != 0) continue;
            reached[ni] = 1;
            ++count;
            for (const std::size_t c : asset.nodes[ni].children) stack.push_back(c);
        }
        if (count != n_nodes) return fail("glTF: node hierarchy contains a cycle");
    }

    for (const fastgltf::Skin& skin : asset.skins) {
        for (const std::size_t j : skin.joints) {
            if (j >= n_nodes) return fail("glTF: skin joint index out of range");
        }
        if (skin.inverseBindMatrices.has_value() && skin.inverseBindMatrices.value() >= n_acc) {
            return fail("glTF: inverse bind matrix accessor out of range");
        }
    }
    for (const fastgltf::Animation& anim : asset.animations) {
        for (const fastgltf::AnimationSampler& smp : anim.samplers) {
            if (smp.inputAccessor >= n_acc || smp.outputAccessor >= n_acc) {
                return fail("glTF: animation sampler accessor out of range");
            }
        }
        for (const fastgltf::AnimationChannel& ch : anim.channels) {
            if (ch.samplerIndex >= anim.samplers.size() ||
                (ch.nodeIndex.has_value() && ch.nodeIndex.value() >= n_nodes)) {
                return fail("glTF: animation channel reference out of range");
            }
        }
    }
    for (const fastgltf::Scene& sc : asset.scenes) {
        for (const std::size_t ni : sc.nodeIndices) {
            if (ni >= n_nodes) return fail("glTF: scene node index out of range");
        }
    }
    return {};
}

std::expected<fastgltf::Asset, core::Error>
parse_gltf(fastgltf::GltfDataBuffer& data, const std::filesystem::path& base_dir) {
    fastgltf::Parser parser(kParserExtensions);
    fastgltf::Expected<fastgltf::Asset> parsed = parser.loadGltf(data, base_dir, kLoadOptions);
    if (parsed.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    fastgltf::Asset asset = std::move(parsed.get());
    if (auto decoded = decode_meshopt_buffers(asset); !decoded) {
        return std::unexpected(decoded.error());
    }
    if (auto valid = validate_asset(asset); !valid) return std::unexpected(valid.error());
    return asset;
}

} // namespace gltf_detail

} // namespace engine::scene
