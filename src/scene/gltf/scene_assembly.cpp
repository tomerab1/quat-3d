// glTF scene graph -> ECS entities (meshes, materials, skins, animators).

#define GLM_ENABLE_EXPERIMENTAL

#include "engine/scene/gltf_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
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

// ---------------------------------------------------------------------------
// Scene instantiation
// ---------------------------------------------------------------------------

std::expected<void, core::Error> validate_skin_bindings(const fastgltf::Asset& a) {
    // Per mesh: the largest weighted joint index over its triangle primitives
    // (computed on first use; most meshes are not skinned by any node).
    std::vector<std::optional<std::optional<std::uint32_t>>> mesh_max(a.meshes.size());
    for (std::size_t ni = 0; ni < a.nodes.size(); ++ni) {
        const fastgltf::Node& n = a.nodes[ni];
        if (!n.meshIndex.has_value() || !n.skinIndex.has_value()) continue;
        const std::size_t m = n.meshIndex.value();
        if (!mesh_max[m].has_value()) {
            std::optional<std::uint32_t> max_joint;
            for (const fastgltf::Primitive& prim : a.meshes[m].primitives) {
                if (prim.type != fastgltf::PrimitiveType::Triangles) continue;
                if (const auto pm = max_weighted_joint(a, prim)) {
                    max_joint = max_joint ? std::max(*max_joint, *pm) : *pm;
                }
            }
            mesh_max[m] = max_joint;
        }
        const std::size_t joint_count = a.skins[n.skinIndex.value()].joints.size();
        if (const std::optional<std::uint32_t> used = *mesh_max[m];
            used.has_value() && *used >= joint_count) {
            return fail("glTF: node " + std::to_string(ni) + " skins mesh " + std::to_string(m) +
                        " whose JOINTS_0 uses joint " + std::to_string(*used) + " but skin " +
                        std::to_string(n.skinIndex.value()) + " has " +
                        std::to_string(joint_count) + " joints");
        }
    }
    return {};
}

// Build ECS entities from a parsed glTF asset. File-local so both the path-based
// public entry and the in-memory self-test share it. `key_prefix` namespaces the
// AssetManager cache keys (so two files' meshes/materials don't collide).
std::expected<std::vector<entt::entity>, core::Error>
assemble_scene(const fastgltf::Asset& a, const std::string& key_prefix,
               rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
               asset::AssetManager& assets, Scene& scene) {
    // Checked before anything is uploaded or cached.
    if (auto ok = validate_skin_bindings(a); !ok) return std::unexpected(ok.error());

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
        textures.thickness = tex_or_null(md.thickness_image);
        const std::string k = key_prefix + "#mat" + std::to_string(i);
        mat_handles[i] = assets.load<asset::MaterialAsset>(
            k, [&](const std::filesystem::path&) {
                return asset::upload_material(md.params, textures, allocator, transfer);
            });
    }

    // Mesh primitives: one MeshAsset (single submesh) + material handle each.
    // The cache keys ride along for MeshSource (scene serialization).
    struct PrimRef {
        asset::AssetHandle<asset::MeshAsset>     mesh;
        asset::AssetHandle<asset::MaterialAsset> material;
        std::string                              mesh_key;
        std::string                              material_key;
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
            ref.mesh_key = k;
            if (prim.materialIndex.has_value()) {
                ref.material = mat_handles[prim.materialIndex.value()];
                ref.material_key =
                    key_prefix + "#mat" + std::to_string(prim.materialIndex.value());
            }
            mesh_prims[m].push_back(ref);
        }
    }

    // Skeletons + animation clips, cached in the AssetManager. A skinned node
    // gets a SkinnedMesh (its skeleton) + an Animator playing the first clip.
    const std::vector<animation::SkeletonAsset> skeletons = extract_skeletons(a);
    std::vector<asset::AssetHandle<animation::SkeletonAsset>> skel_handles(skeletons.size());
    for (std::size_t i = 0; i < skeletons.size(); ++i) {
        skel_handles[i] = assets.load<animation::SkeletonAsset>(
            key_prefix + "#skin" + std::to_string(i),
            [&](const std::filesystem::path&) { return skeletons[i]; });
    }
    const std::vector<animation::AnimClipAsset> clips = extract_animations(a);
    std::vector<asset::AssetHandle<animation::AnimClipAsset>> clip_handles(clips.size());
    for (std::size_t i = 0; i < clips.size(); ++i) {
        clip_handles[i] = assets.load<animation::AnimClipAsset>(
            key_prefix + "#anim" + std::to_string(i),
            [&](const std::filesystem::path&) { return clips[i]; });
    }

    // Each skin's armature transform: the global transform of the parent of its
    // root joint, so the skeleton is placed correctly when it lives under a
    // transformed (e.g. Z-up -> Y-up) node.
    std::vector<int> parent_node(a.nodes.size(), -1);
    for (std::size_t ni = 0; ni < a.nodes.size(); ++ni) {
        for (std::size_t c : a.nodes[ni].children) {
            parent_node[c] = static_cast<int>(ni);
        }
    }
    std::vector<glm::mat4> node_global(a.nodes.size(), glm::mat4(1.0F));
    std::vector<char> node_done(a.nodes.size(), 0);
    std::function<glm::mat4(int)> node_global_of = [&](int ni) -> glm::mat4 {
        if (ni < 0) return glm::mat4(1.0F);
        if (node_done[ni] == 0) {
            node_global[ni] = node_global_of(parent_node[ni]) * node_local_matrix(a.nodes[ni]);
            node_done[ni] = 1;
        }
        return node_global[ni];
    };
    std::vector<glm::mat4> skin_root_transform(skeletons.size(), glm::mat4(1.0F));
    for (std::size_t i = 0; i < skeletons.size(); ++i) {
        for (const animation::Joint& j : skeletons[i].joints) {
            if (j.parent < 0) {
                skin_root_transform[i] = node_global_of(parent_node[j.node]);
                break;
            }
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

            // Attach skin + animator to whichever entity carries the MeshRenderer.
            const auto attach_skin = [&](entt::entity ent) {
                if (n.skinIndex.has_value() && n.skinIndex.value() < skel_handles.size()) {
                    SkinnedMesh& sm = reg.emplace<SkinnedMesh>(
                        ent, skel_handles[n.skinIndex.value()], std::vector<glm::mat4>{});
                    // Rebase the armature global into this entity's frame: the
                    // animation system multiplies the entity's Transform.world on
                    // top each tick (so editing the transform moves the model).
                    // For an untouched instance world == the mesh node's glTF
                    // global, and the product reduces to the armature global.
                    sm.root_transform =
                        glm::inverse(node_global_of(static_cast<int>(node_index))) *
                        skin_root_transform[n.skinIndex.value()];
                    if (!clip_handles.empty()) {
                        Animator& anim = reg.emplace<Animator>(ent);
                        anim.clip = clip_handles[0];
                        anim.clips = clip_handles; // what the animator can switch to
                    }
                }
            };

            if (n.meshIndex.has_value()) {
                const std::vector<PrimRef>& prims = mesh_prims[n.meshIndex.value()];
                if (prims.size() == 1) {
                    reg.emplace<MeshRenderer>(e, prims[0].mesh, prims[0].material);
                    reg.emplace<MeshSource>(e, prims[0].mesh_key, prims[0].material_key,
                                            key_prefix, static_cast<int>(node_index));
                    attach_skin(e);
                } else {
                    for (std::size_t p = 0; p < prims.size(); ++p) {
                        const entt::entity pe = scene.create_entity(
                            std::string(n.name.begin(), n.name.end()) + ".prim" +
                            std::to_string(p));
                        scene.set_parent(pe, e);
                        reg.emplace<MeshRenderer>(pe, prims[p].mesh, prims[p].material);
                        reg.emplace<MeshSource>(pe, prims[p].mesh_key, prims[p].material_key,
                                                key_prefix, static_cast<int>(node_index));
                        attach_skin(pe);
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


} // namespace gltf_detail

using namespace gltf_detail;

std::expected<std::vector<entt::entity>, core::Error>
GltfLoader::instantiate(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                        const rhi::TransferContext& transfer, asset::AssetManager& assets,
                        Scene& scene) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    fastgltf::Parser parser(kParserExtensions);
    fastgltf::Expected<fastgltf::Asset> parsed =
        parser.loadGltf(data.get(), path.parent_path(), kLoadOptions);
    if (parsed.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    if (auto valid = validate_asset(parsed.get()); !valid) return std::unexpected(valid.error());
    return assemble_scene(parsed.get(), path.generic_string(), allocator, transfer, assets, scene);
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

} // namespace engine::scene
