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

// Structural validation of a parsed asset, run before any extraction. glTF
// files are untrusted input: every cross-reference the loader follows is
// range-checked here, attribute accessors must agree with POSITION's count
// (they are written by index into arrays sized from it), and the node graph
// must be a forest (each node has at most one parent, no cycles) because
// scene assembly, skin extraction and world-matrix resolution recurse over
// it. fastgltf::validate covers the spec's general rules on top.
[[nodiscard]] std::expected<void, core::Error> validate_asset(const fastgltf::Asset& asset) {
    if (const fastgltf::Error e = fastgltf::validate(asset); e != fastgltf::Error::None) {
        return fail(std::string("glTF failed validation: ") +
                    std::string(fastgltf::getErrorMessage(e)));
    }
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

    const auto check_texture = [&](const auto& info) -> bool {
        if (!info.has_value()) return true;
        if (info->textureIndex >= asset.textures.size()) return false;
        const fastgltf::Texture& tex = asset.textures[info->textureIndex];
        return !tex.imageIndex.has_value() || tex.imageIndex.value() < asset.images.size();
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


} // namespace gltf_detail

} // namespace engine::scene
