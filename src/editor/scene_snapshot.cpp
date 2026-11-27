#include "scene_snapshot.hpp"

#include <cstdint>
#include <string>
#include <type_traits>

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/scene/scene.hpp"

namespace engine::editor {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

template <typename... Ts, typename F>
void for_each_component(ComponentList<Ts...> /*list*/, F&& f) {
    (f.template operator()<Ts>(), ...);
}

// Clears what belongs to the session that created it, not to the authored
// scene (see capture_scene / duplicate_entity).
void clear_audio_runtime(scene::AudioSource& src) {
    src.command = scene::AudioCommand::none;
    src.state = audio::Playback::stopped;
    src.started = false;
    src.sound = audio::invalid_sound;
    src.resolved = 0;
}

} // namespace

SceneSnapshot capture_scene(const scene::Scene& scene) {
    const entt::registry& r = scene.registry();
    SceneSnapshot snap;

    // Every live entity, so even component-less ones survive the round trip.
    for (const entt::entity e : r.view<entt::entity>()) {
        snap.entities.push_back(e);
    }

    for_each_component(SnapshotComponents{}, [&]<typename T>() {
        auto& out = snap.stored<T>();
        for (const auto [e, component] : r.view<const T>().each()) {
            out.emplace_back(e, component);
        }
    });
    // Sounds belong to the session that made them: restored sources start
    // without one (the audio system's sweep frees the play session's sounds)
    // and play_on_start fires again on the next Play.
    for (auto& [e, src] : snap.stored<scene::AudioSource>()) clear_audio_runtime(src);
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

    for_each_component(SnapshotComponents{}, [&]<typename T>() {
        for (const auto& [e, component] : snapshot.stored<T>()) {
            r.emplace<T>(e, component);
        }
    });
}

entt::entity duplicate_entity(scene::Scene& scene, entt::entity source) {
    entt::registry& r = scene.registry();
    if (!r.valid(source)) return entt::null;
    std::string label;
    if (const auto* name = r.try_get<scene::Name>(source); name != nullptr && !name->value.empty()) {
        label = name->value;
    } else {
        label = "entity " + std::to_string(static_cast<std::uint32_t>(source));
    }
    const entt::entity copy = scene.create_entity(label + " copy");

    for_each_component(SnapshotComponents{}, [&]<typename T>() {
        // Name is set above; hierarchy links are rebuilt through set_parent
        // (the copy has no children of its own).
        if constexpr (!std::is_same_v<T, scene::Name> && !std::is_same_v<T, scene::Parent> &&
                      !std::is_same_v<T, scene::Children>) {
            if (const T* c = r.try_get<T>(source)) {
                const T value = *c; // copy first: emplacing may reallocate the pool
                r.emplace_or_replace<T>(copy, value);
            }
        }
    });

    // Runtime state the copy must not share with its source.
    if (auto* rb = r.try_get<scene::RigidBody>(copy)) rb->body = scene::RigidBody{}.body;
    if (auto* cc = r.try_get<scene::CharacterController>(copy)) {
        const scene::CharacterController fresh;
        cc->character = fresh.character;
        cc->velocity = fresh.velocity;
        cc->on_ground = fresh.on_ground;
    }
    if (auto* src = r.try_get<scene::AudioSource>(copy)) clear_audio_runtime(*src);
    if (auto* net = r.try_get<scene::NetReplicated>(copy)) net->net_id = 0;

    if (const auto* p = r.try_get<scene::Parent>(source); p != nullptr && p->entity != entt::null) {
        scene.set_parent(copy, p->entity);
    }
    return copy;
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
    {
        scene::Terrain terrain;
        terrain.params.seed = 99;
        terrain.regenerate = false; // restored as-is: tiles persist across Stop
        r.emplace<scene::Terrain>(parent, terrain);
    }

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
    const auto* terrain = r.try_get<scene::Terrain>(parent);
    if (terrain == nullptr || terrain->params.seed != 99) {
        return fail("snapshot self-test: terrain component lost across play/stop");
    }
    return {};
}

} // namespace engine::editor
