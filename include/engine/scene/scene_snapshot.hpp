#pragma once

// Scene snapshot for play mode (Phase 9.9): the editor's Play/Stop and the
// non-editor game's play sessions (app PlayMode), and the editor's Duplicate.
//
// capture() copies every entity and every engine component into plain vectors;
// restore() clears the registry and rebuilds it with the EXACT same entity
// identifiers (entt's create(hint) honours id + version on a fresh registry),
// so stored entt::entity references — Parent/Children, the editor selection —
// stay valid across a play/stop round trip.

#include <expected>
#include <tuple>
#include <utility>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/scene/world_clock.hpp"

namespace engine::scene {

class Scene;

template <typename... Ts>
struct ComponentList {};

// Every engine component the editor copies as a whole: the play-mode snapshot
// (capture/restore) and the hierarchy's Duplicate both walk this one list, so
// a component added here is covered by both. Runtime-only tags (NetDriven)
// are deliberately absent.
using SnapshotComponents =
    ComponentList<scene::Name, scene::Transform, scene::Parent, scene::Children,
                  scene::MeshRenderer, scene::MaterialOverride, scene::MeshSource,
                  scene::SkinnedMesh, scene::Animator, scene::FootIK, scene::Camera,
                  scene::DirectionalLight, scene::PointLight, scene::Collider,
                  scene::RigidBody, scene::Joint, scene::CharacterController, scene::Terrain,
                  scene::Grass, scene::ParticleEmitter, scene::WorldClock,
                  scene::NavAgent, scene::BehaviorTree, scene::PatrolRoute, scene::Primitive,
                  scene::InstancedMesh, scene::InstancedMeshParts, scene::Scatter,
                  scene::NetReplicated, scene::AudioSource, scene::AudioListener,
                  scene::Script>;

namespace snapshot_detail {
template <typename List>
struct StoredTuple;
template <typename... Ts>
struct StoredTuple<ComponentList<Ts...>> {
    using type = std::tuple<std::vector<std::pair<entt::entity, Ts>>...>;
};
} // namespace snapshot_detail

struct SceneSnapshot {
    std::vector<entt::entity> entities;
    // Scene-level settings kept in the registry context (A10.1 gravity).
    scene::PhysicsSettings    physics_settings{};

    template <typename T>
    using Stored = std::vector<std::pair<entt::entity, T>>;

    // One Stored<T> per SnapshotComponents type.
    snapshot_detail::StoredTuple<SnapshotComponents>::type components;

    template <typename T>
    [[nodiscard]] Stored<T>& stored() {
        return std::get<Stored<T>>(components);
    }
    template <typename T>
    [[nodiscard]] const Stored<T>& stored() const {
        return std::get<Stored<T>>(components);
    }
};

// Copies the whole scene (all engine components) into a snapshot.
[[nodiscard]] SceneSnapshot capture_scene(const scene::Scene& scene);

// Clears the registry and rebuilds it from the snapshot with identical entity
// identifiers. Anything created during play disappears; anything destroyed
// comes back.
void restore_scene(scene::Scene& scene, const SceneSnapshot& snapshot);

// Clones `source` onto a new sibling entity (same parent, name + " copy"):
// every SnapshotComponents component except the hierarchy links, with
// runtime state the copy must not share reset — physics body / character
// handles (the systems create its own), audio sound handles and playback
// state, and the network id (reassigned on the next session start). Children
// are not cloned. Returns the copy (entt::null if `source` is invalid).
entt::entity duplicate_entity(scene::Scene& scene, entt::entity source);

// Round-trip self-test: capture, mutate/destroy/create, restore, verify the
// original entities (ids included) and component values are back.
[[nodiscard]] std::expected<void, core::Error> run_scene_snapshot_self_test();

} // namespace engine::scene
