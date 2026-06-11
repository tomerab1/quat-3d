#pragma once

// Scene snapshot for the editor's play mode (Phase 9.9). Private editor header.
//
// capture() copies every entity and every engine component into plain vectors;
// restore() clears the registry and rebuilds it with the EXACT same entity
// identifiers (entt's create(hint) honours id + version on a fresh registry),
// so stored entt::entity references — Parent/Children, the editor selection —
// stay valid across a play/stop round trip.

#include <expected>
#include <utility>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {
class Scene;
}

namespace engine::editor {

struct SceneSnapshot {
    std::vector<entt::entity> entities;

    template <typename T>
    using Stored = std::vector<std::pair<entt::entity, T>>;

    Stored<scene::Name>                names;
    Stored<scene::Transform>           transforms;
    Stored<scene::Parent>              parents;
    Stored<scene::Children>            children;
    Stored<scene::MeshRenderer>        mesh_renderers;
    Stored<scene::MeshSource>          mesh_sources;
    Stored<scene::SkinnedMesh>         skinned_meshes;
    Stored<scene::Animator>            animators;
    Stored<scene::Camera>              cameras;
    Stored<scene::DirectionalLight>    directional_lights;
    Stored<scene::PointLight>          point_lights;
    Stored<scene::Collider>            colliders;
    Stored<scene::RigidBody>           rigid_bodies;
    Stored<scene::CharacterController> characters;
    Stored<scene::Terrain>             terrains;
};

// Copies the whole scene (all engine components) into a snapshot.
[[nodiscard]] SceneSnapshot capture_scene(const scene::Scene& scene);

// Clears the registry and rebuilds it from the snapshot with identical entity
// identifiers. Anything created during play disappears; anything destroyed
// comes back.
void restore_scene(scene::Scene& scene, const SceneSnapshot& snapshot);

// Round-trip self-test: capture, mutate/destroy/create, restore, verify the
// original entities (ids included) and component values are back.
[[nodiscard]] std::expected<void, core::Error> run_scene_snapshot_self_test();

} // namespace engine::editor
