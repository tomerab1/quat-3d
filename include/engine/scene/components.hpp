#pragma once

// Core ECS components (Phase 4, Slice 4.1).
//
// Plain-old-data components driven by systems in `scene.cpp`. Per CLAUDE.md:
// components carry no logic — that lives in systems. Only the components needed
// through Phase 4 are defined here (Transform, MeshRenderer, Camera,
// DirectionalLight, Name, Parent, Children); skinning/physics/light components
// arrive with their respective phases.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"

namespace engine::scene {

// Local transform plus its resolved world-space matrix. `local` is authored
// (or composed from glTF TRS at load); `world` is produced each tick by
// TransformSystem as parent.world * local (identity parent for roots).
struct Transform {
    glm::mat4 local{1.0F};
    glm::mat4 world{1.0F};
};

// A renderable: which mesh to draw with which material. Null/unloaded handles
// resolve to the manager's per-type defaults, so a partially-built entity still
// renders something rather than crashing.
struct MeshRenderer {
    asset::AssetHandle<asset::MeshAsset>     mesh;
    asset::AssetHandle<asset::MaterialAsset> material;
};

// Marks an entity (alongside its MeshRenderer) as skinned: its mesh deforms with
// `skeleton`. `joint_matrices` holds the per-joint skinning matrices the
// animation system writes each frame (joint_world * inverse_bind); the GPU
// skinning pass (5.4) consumes them. Empty until an AnimationSystem fills it.
struct SkinnedMesh {
    asset::AssetHandle<animation::SkeletonAsset> skeleton;
    std::vector<glm::mat4>                       joint_matrices;
    // World transform of the skeleton's root ancestor (the "armature" node above
    // the joints). The skeleton hierarchy is resolved relative to this, so it
    // must be folded into the joint matrices for the character to sit in the
    // right place. Identity when the skeleton roots at the scene root.
    glm::mat4                                    root_transform{1.0F};
};

// Plays an animation clip on a skinned entity. The AnimationSystem advances
// `time` by dt*speed each tick (wrapping when `looping`), samples the clip, and
// writes the result into the entity's SkinnedMesh::joint_matrices.
struct Animator {
    asset::AssetHandle<animation::AnimClipAsset> clip;
    float time    = 0.0F;
    float speed   = 1.0F;
    bool  looping = true;
};

// Perspective camera parameters. The view/projection matrices are derived each
// frame by CameraSystem from this plus the entity's world Transform (4.3).
// Aspect ratio is supplied by the renderer from the swapchain extent.
struct Camera {
    float fov_y  = glm::radians(60.0F); // vertical field of view, radians
    float near_z = 0.1F;
    float far_z  = 1000.0F;
    bool  is_active = true;
};

// Single directional light (sun). `direction` points from the light toward the
// scene (i.e. the direction photons travel) in world space.
struct DirectionalLight {
    glm::vec3 direction{0.0F, -1.0F, 0.0F};
    glm::vec3 color{1.0F};
    float     intensity = 1.0F;
};

// Human-readable label. Editor-only metadata, not on any hot path.
struct Name {
    std::string value;
};

// Scene-graph parent link. `entity` is entt::null for a root. Children are kept
// in sync via the Children component on the parent.
struct Parent {
    entt::entity entity = entt::null;
};

// Scene-graph child list. Maintained alongside Parent so the hierarchy can be
// walked in either direction without a full registry scan.
struct Children {
    std::vector<entt::entity> entities;
};

// Exercises entt integration: spawns entities, attaches the core components,
// and verifies view/group queries observe them. Proves the ECS world links and
// the component types are registry-storable. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_ecs_self_test();

} // namespace engine::scene
