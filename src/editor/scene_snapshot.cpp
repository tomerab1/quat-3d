#include "scene_snapshot.hpp"

#include <string>

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/scene/scene.hpp"

namespace engine::editor {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

template <typename T>
void capture_type(const entt::registry& registry, SceneSnapshot::Stored<T>& out) {
    for (const auto [e, component] : registry.view<const T>().each()) {
        out.emplace_back(e, component);
    }
}

template <typename T>
void restore_type(entt::registry& registry, const SceneSnapshot::Stored<T>& in) {
    for (const auto& [e, component] : in) {
        registry.emplace<T>(e, component);
    }
}

} // namespace

SceneSnapshot capture_scene(const scene::Scene& scene) {
    const entt::registry& r = scene.registry();
    SceneSnapshot snap;

    // Every live entity, so even component-less ones survive the round trip.
    for (const entt::entity e : r.view<entt::entity>()) {
        snap.entities.push_back(e);
    }

    capture_type(r, snap.names);
    capture_type(r, snap.transforms);
    capture_type(r, snap.parents);
    capture_type(r, snap.children);
    capture_type(r, snap.mesh_renderers);
    capture_type(r, snap.mesh_sources);
    capture_type(r, snap.skinned_meshes);
    capture_type(r, snap.animators);
    capture_type(r, snap.cameras);
    capture_type(r, snap.directional_lights);
    capture_type(r, snap.point_lights);
    capture_type(r, snap.colliders);
    capture_type(r, snap.rigid_bodies);
    capture_type(r, snap.characters);
    return snap;
}

void restore_scene(scene::Scene& scene, const SceneSnapshot& snapshot) {
    entt::registry& r = scene.registry();
    r.clear();

    // create(hint) honours both id and version when the identifier is free —
    // guaranteed here because the registry was just cleared and re-created
    // identifiers come back exactly as captured.
    for (const entt::entity e : snapshot.entities) {
        (void)r.create(e);
    }

    restore_type(r, snapshot.names);
    restore_type(r, snapshot.transforms);
    restore_type(r, snapshot.parents);
    restore_type(r, snapshot.children);
    restore_type(r, snapshot.mesh_renderers);
    restore_type(r, snapshot.mesh_sources);
    restore_type(r, snapshot.skinned_meshes);
    restore_type(r, snapshot.animators);
    restore_type(r, snapshot.cameras);
    restore_type(r, snapshot.directional_lights);
    restore_type(r, snapshot.point_lights);
    restore_type(r, snapshot.colliders);
    restore_type(r, snapshot.rigid_bodies);
    restore_type(r, snapshot.characters);
}

std::expected<void, core::Error> run_scene_snapshot_self_test() {
    scene::Scene scene;
    entt::registry& r = scene.registry();

    // A small hierarchy with values to verify.
    const entt::entity parent = scene.create_entity("parent");
    const entt::entity child = scene.create_entity("child");
    scene.set_parent(child, parent);
    r.get<scene::Transform>(parent).local = glm::translate(glm::mat4(1.0F), {1.0F, 2.0F, 3.0F});
    r.emplace<scene::Collider>(parent, scene::ColliderShape::sphere, glm::vec3(0.75F),
                               glm::vec3(0.0F));
    r.emplace<scene::RigidBody>(parent, scene::BodyMotion::dynamic, 2.5F);
    r.emplace<scene::PointLight>(child, glm::vec3(1.0F, 0.5F, 0.25F), 7.0F, 3.0F);

    const SceneSnapshot snap = capture_scene(scene);

    // Simulate a destructive play session: move, retype, destroy, create.
    r.get<scene::Transform>(parent).local = glm::mat4(1.0F);
    r.get<scene::RigidBody>(parent).body = 42;
    r.destroy(child);
    const entt::entity intruder = scene.create_entity("intruder");
    (void)intruder;

    restore_scene(scene, snap);

    if (!r.valid(parent) || !r.valid(child)) {
        return fail("snapshot self-test: original entities did not come back");
    }
    if (r.view<scene::Transform>().size() != 2) {
        return fail("snapshot self-test: play-created entity survived the restore");
    }
    const auto* name = r.try_get<scene::Name>(child);
    if (name == nullptr || name->value != "child") {
        return fail("snapshot self-test: child name lost");
    }
    const auto* p = r.try_get<scene::Parent>(child);
    if (p == nullptr || p->entity != parent) {
        return fail("snapshot self-test: hierarchy link broken (entity id not preserved)");
    }
    if (r.get<scene::Transform>(parent).local[3][0] != 1.0F) {
        return fail("snapshot self-test: transform value not restored");
    }
    if (r.get<scene::RigidBody>(parent).body != 0xFFFFFFFFU) {
        return fail("snapshot self-test: rigid body handle not restored");
    }
    if (r.get<scene::Collider>(parent).shape != scene::ColliderShape::sphere) {
        return fail("snapshot self-test: collider not restored");
    }
    return {};
}

} // namespace engine::editor
