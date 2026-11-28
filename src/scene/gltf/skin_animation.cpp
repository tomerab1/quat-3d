// glTF skins (skeletons) and animation clips.

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

// Convert a fastgltf column-major 4x4 to glm (also column-major).
[[nodiscard]] glm::mat4 to_glm_mat4(const fastgltf::math::fmat4x4& m) {
    glm::mat4 out(1.0F);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c][r] = m[c][r];
        }
    }
    return out;
}

namespace {
// Flatten one glTF skin into a SkeletonAsset: joint names, parent indices within
// the joint set, bind-pose local TRS (from each joint node), and inverse bind
// matrices.
[[nodiscard]] animation::SkeletonAsset extract_skin(const fastgltf::Asset& asset,
                                                    const fastgltf::Skin& skin) {
    animation::SkeletonAsset out;
    const std::size_t n = skin.joints.size();
    out.joints.resize(n);

    // node index -> joint index, and node -> parent node over the whole graph.
    std::unordered_map<std::size_t, int> node_to_joint;
    for (std::size_t i = 0; i < n; ++i) {
        node_to_joint[skin.joints[i]] = static_cast<int>(i);
    }
    std::vector<int> parent_node(asset.nodes.size(), -1);
    for (std::size_t ni = 0; ni < asset.nodes.size(); ++ni) {
        for (std::size_t child : asset.nodes[ni].children) {
            parent_node[child] = static_cast<int>(ni);
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t node_index = skin.joints[i];
        const fastgltf::Node& node = asset.nodes[node_index];
        animation::Joint& j = out.joints[i];
        j.name.assign(node.name.begin(), node.name.end());
        j.node = static_cast<int>(node_index);

        const int pn = parent_node[node_index];
        const auto it =
            pn >= 0 ? node_to_joint.find(static_cast<std::size_t>(pn)) : node_to_joint.end();
        j.parent = it != node_to_joint.end() ? it->second : -1;

        std::visit(fastgltf::visitor{
                       [&](const fastgltf::TRS& trs) {
                           j.translation = to_glm(trs.translation);
                           // fastgltf quaternion is [x,y,z,w]; glm::quat is (w,x,y,z).
                           j.rotation = glm::quat(trs.rotation[3], trs.rotation[0],
                                                  trs.rotation[1], trs.rotation[2]);
                           j.scale = to_glm(trs.scale);
                       },
                       [&](const fastgltf::math::fmat4x4& m) {
                           glm::vec3 skew;
                           glm::vec4 perspective;
                           glm::decompose(to_glm_mat4(m), j.scale, j.rotation, j.translation, skew,
                                          perspective);
                       }},
                   node.transform);
    }

    if (skin.inverseBindMatrices.has_value()) {
        const fastgltf::Accessor& acc = asset.accessors[skin.inverseBindMatrices.value()];
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
            asset, acc, [&](fastgltf::math::fmat4x4 m, std::size_t idx) {
                if (idx < out.joints.size()) {
                    out.joints[idx].inverse_bind = to_glm_mat4(m);
                }
            });
    }
    return out;
}
} // namespace

[[nodiscard]] std::vector<animation::SkeletonAsset>
extract_skeletons(const fastgltf::Asset& asset) {
    std::vector<animation::SkeletonAsset> out;
    out.reserve(asset.skins.size());
    for (const fastgltf::Skin& skin : asset.skins) {
        out.push_back(extract_skin(asset, skin));
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<animation::SkeletonAsset>, core::Error>
parse_and_extract_skeletons(fastgltf::GltfDataBuffer& data,
                            const std::filesystem::path& base_dir) {
    auto asset = parse_gltf(data, base_dir);
    if (!asset) return std::unexpected(asset.error());
    return extract_skeletons(*asset);
}

namespace {
// Flatten one glTF animation into an AnimClipAsset. CUBICSPLINE samplers keep
// only their value keyframes (dropping the in/out tangents) and become LINEAR.
[[nodiscard]] animation::AnimClipAsset extract_animation(const fastgltf::Asset& asset,
                                                         const fastgltf::Animation& anim) {
    animation::AnimClipAsset clip;
    clip.name.assign(anim.name.begin(), anim.name.end());

    clip.samplers.reserve(anim.samplers.size());
    for (const fastgltf::AnimationSampler& smp : anim.samplers) {
        animation::AnimSampler out;
        const fastgltf::Accessor& in_acc = asset.accessors[smp.inputAccessor];
        out.input.reserve(in_acc.count);
        fastgltf::iterateAccessor<float>(asset, in_acc,
                                         [&](float v) { out.input.push_back(v); });

        const bool cubic = smp.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
        out.interpolation = smp.interpolation == fastgltf::AnimationInterpolation::Step
                                ? animation::Interpolation::step
                                : animation::Interpolation::linear;

        const fastgltf::Accessor& out_acc = asset.accessors[smp.outputAccessor];
        std::vector<glm::vec4> raw;
        raw.reserve(out_acc.count);
        if (out_acc.type == fastgltf::AccessorType::Vec4) {
            fastgltf::iterateAccessor<fastgltf::math::fvec4>(
                asset, out_acc, [&](fastgltf::math::fvec4 v) { raw.push_back(to_glm(v)); });
        } else {
            fastgltf::iterateAccessor<fastgltf::math::fvec3>(
                asset, out_acc,
                [&](fastgltf::math::fvec3 v) { raw.emplace_back(to_glm(v), 0.0F); });
        }

        if (cubic) {
            // [inTangent, value, outTangent] per key -> keep the values only.
            std::vector<glm::vec4> values;
            values.reserve(out.input.size());
            for (std::size_t k = 0; k < out.input.size(); ++k) {
                const std::size_t vi = k * 3 + 1;
                if (vi < raw.size()) values.push_back(raw[vi]);
            }
            out.output = std::move(values);
        } else {
            out.output = std::move(raw);
        }

        // A sampler's output must hold one value per input key. Malformed
        // files can disagree; keep only the keys that have a value (the
        // sampler also clamps, but the duration must not outrun the data).
        if (out.output.size() != out.input.size()) {
            std::fprintf(stderr,
                         "[gltf] animation '%s': sampler has %zu keys but %zu values; "
                         "truncating\n",
                         clip.name.c_str(), out.input.size(), out.output.size());
            const std::size_t n = std::min(out.input.size(), out.output.size());
            out.input.resize(n);
            out.output.resize(n);
        }
        if (!out.input.empty()) {
            clip.duration = std::max(clip.duration, out.input.back());
        }
        clip.samplers.push_back(std::move(out));
    }

    clip.channels.reserve(anim.channels.size());
    for (const fastgltf::AnimationChannel& ch : anim.channels) {
        animation::AnimChannel out;
        out.target_node = ch.nodeIndex.has_value() ? static_cast<int>(ch.nodeIndex.value()) : -1;
        out.sampler = ch.samplerIndex;
        switch (ch.path) {
        case fastgltf::AnimationPath::Translation:
            out.path = animation::AnimPath::translation;
            break;
        case fastgltf::AnimationPath::Rotation:
            out.path = animation::AnimPath::rotation;
            break;
        case fastgltf::AnimationPath::Scale:
            out.path = animation::AnimPath::scale;
            break;
        default:
            continue; // morph-target weights unsupported
        }
        clip.channels.push_back(out);
    }
    return clip;
}
} // namespace

[[nodiscard]] std::vector<animation::AnimClipAsset>
extract_animations(const fastgltf::Asset& asset) {
    std::vector<animation::AnimClipAsset> out;
    out.reserve(asset.animations.size());
    for (const fastgltf::Animation& anim : asset.animations) {
        out.push_back(extract_animation(asset, anim));
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<animation::AnimClipAsset>, core::Error>
parse_and_extract_animations(fastgltf::GltfDataBuffer& data,
                             const std::filesystem::path& base_dir) {
    auto asset = parse_gltf(data, base_dir);
    if (!asset) return std::unexpected(asset.error());
    return extract_animations(*asset);
}


} // namespace gltf_detail

using namespace gltf_detail;

std::expected<std::vector<animation::SkeletonAsset>, core::Error>
GltfLoader::load_skeletons(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + core::path_to_utf8(path) + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_skeletons(data.get(), path.parent_path());
}

std::expected<std::vector<animation::SkeletonAsset>, core::Error>
GltfLoader::load_skeletons_from_memory(std::span<const std::byte> bytes,
                                       const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_skeletons(data.get(), base_dir);
}

std::expected<std::vector<animation::AnimClipAsset>, core::Error>
GltfLoader::load_animations(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + core::path_to_utf8(path) + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_animations(data.get(), path.parent_path());
}

std::expected<std::vector<animation::AnimClipAsset>, core::Error>
GltfLoader::load_animations_from_memory(std::span<const std::byte> bytes,
                                        const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_animations(data.get(), base_dir);
}

} // namespace engine::scene
