// Play-mode snapshot (capture on Play, restore on Stop).

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <type_traits>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "editor/scene_snapshot.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/scene/scene.hpp"

namespace {

namespace s = engine::scene;

// Audit B12/C9: Stop used to drop NetReplicated (replication silently ended
// after one Play/Stop cycle); Primitive must survive the same way.
TEST(PlayModeSnapshot, RestoresNetReplicatedAndPrimitive) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("crate");
    scene.registry().emplace<s::NetReplicated>(e, 7U);
    scene.registry().emplace<s::Primitive>(e).shape = s::PrimitiveShape::cone;

    const auto snapshot = engine::editor::capture_scene(scene);
    scene.registry().remove<s::NetReplicated, s::Primitive>(e);
    engine::editor::restore_scene(scene, snapshot);

    ASSERT_TRUE(scene.registry().valid(e));
    ASSERT_TRUE(scene.registry().all_of<s::NetReplicated>(e));
    EXPECT_EQ(scene.registry().get<s::NetReplicated>(e).net_id, 7U);
    ASSERT_TRUE(scene.registry().all_of<s::Primitive>(e));
    EXPECT_EQ(scene.registry().get<s::Primitive>(e).shape, s::PrimitiveShape::cone);
}

// Audio components survive Play/Stop with their authored fields; the play
// session's sound handle and playback state do not (the audio system's sweep
// frees the sound, and play_on_start fires again on the next Play).
TEST(PlayModeSnapshot, RestoresAudioComponentsWithoutRuntimeState) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("radio");
    s::AudioSource src;
    src.clip = "assets/audio/song.mp3";
    src.bus = engine::audio::AudioBus::music;
    src.volume = 0.5F;
    scene.registry().emplace<s::AudioSource>(e, src);
    scene.registry().emplace<s::AudioListener>(e).active = false;

    const auto snapshot = engine::editor::capture_scene(scene);
    auto& live = scene.registry().get<s::AudioSource>(e);
    live.sound = 42;
    live.started = true;
    live.state = engine::audio::Playback::playing;
    live.volume = 1.0F;
    engine::editor::restore_scene(scene, snapshot);

    ASSERT_TRUE(scene.registry().all_of<s::AudioSource>(e));
    const auto& restored = scene.registry().get<s::AudioSource>(e);
    EXPECT_EQ(restored.clip, "assets/audio/song.mp3");
    EXPECT_EQ(restored.bus, engine::audio::AudioBus::music);
    EXPECT_FLOAT_EQ(restored.volume, 0.5F);
    EXPECT_EQ(restored.sound, engine::audio::invalid_sound);
    EXPECT_FALSE(restored.started);
    EXPECT_EQ(restored.state, engine::audio::Playback::stopped);
    ASSERT_TRUE(scene.registry().all_of<s::AudioListener>(e));
    EXPECT_FALSE(scene.registry().get<s::AudioListener>(e).active);
}

// R9.2: instanced meshes, their resolved parts and scatters survive Play/Stop
// (placements a play session changed come back as authored).
TEST(PlayModeSnapshot, RestoresInstancedMeshAndScatter) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("forest");
    auto& mesh = scene.registry().emplace<s::InstancedMesh>(e);
    mesh.gltf_path = "assets/meshes/tree.glb";
    mesh.instances.resize(12);
    mesh.instances[3].position = glm::vec3(1.0F, 2.0F, 3.0F);
    scene.registry().emplace<s::InstancedMeshParts>(e).resolved = 42;
    auto& scatter = scene.registry().emplace<s::Scatter>(e);
    scatter.seed = 17;
    scatter.generated = 99;
    scatter.generation = 3;

    const auto snapshot = engine::editor::capture_scene(scene);
    scene.registry().get<s::InstancedMesh>(e).instances.clear();
    scene.registry().remove<s::Scatter, s::InstancedMeshParts>(e);
    engine::editor::restore_scene(scene, snapshot);

    auto& r = scene.registry();
    ASSERT_TRUE(r.all_of<s::InstancedMesh>(e));
    EXPECT_EQ(r.get<s::InstancedMesh>(e).gltf_path, "assets/meshes/tree.glb");
    ASSERT_EQ(r.get<s::InstancedMesh>(e).instances.size(), 12U);
    EXPECT_EQ(r.get<s::InstancedMesh>(e).instances[3].position, glm::vec3(1.0F, 2.0F, 3.0F));
    ASSERT_TRUE(r.all_of<s::InstancedMeshParts>(e));
    EXPECT_EQ(r.get<s::InstancedMeshParts>(e).resolved, 42U);
    ASSERT_TRUE(r.all_of<s::Scatter>(e));
    EXPECT_EQ(r.get<s::Scatter>(e).seed, 17U);
    EXPECT_EQ(r.get<s::Scatter>(e).generated, 99U); // no needless regeneration
}

// R9.3: the grass field survives Play/Stop.
TEST(PlayModeSnapshot, RestoresGrass) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("meadow");
    scene.registry().emplace<s::Grass>(e).settings.density = 63.0F;

    const auto snapshot = engine::editor::capture_scene(scene);
    scene.registry().remove<s::Grass>(e);
    engine::editor::restore_scene(scene, snapshot);

    ASSERT_TRUE(scene.registry().all_of<s::Grass>(e));
    EXPECT_EQ(scene.registry().get<s::Grass>(e).settings.density, 63.0F);
}

template <typename... Ts, typename F>
void for_each_listed(engine::editor::ComponentList<Ts...> /*list*/, F&& f) {
    (f.template operator()<Ts>(), ...);
}

// Audit C11: the hierarchy's Duplicate copied only MeshRenderer, lights and
// Camera. It now copies every component the snapshot covers, minus the
// runtime state a copy must not share.
TEST(EditorDuplicate, CopiesEverySnapshotComponent) {
    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity parent = scene.create_entity("group");
    const entt::entity e = scene.create_entity("crate");
    ASSERT_TRUE(scene.set_parent(e, parent));
    // Every listed component (hierarchy links aside) with its defaults...
    for_each_listed(engine::editor::SnapshotComponents{}, [&]<typename T>() {
        if constexpr (!std::is_same_v<T, s::Parent> && !std::is_same_v<T, s::Children>) {
            (void)r.get_or_emplace<T>(e);
        }
    });
    // ...plus authored values and session state to tell copied from reset.
    r.get<s::Transform>(e).local[3] = glm::vec4(1.0F, 2.0F, 3.0F, 1.0F);
    r.get<s::Collider>(e).half_extents = glm::vec3(2.0F);
    r.get<s::RigidBody>(e) = {s::BodyMotion::kinematic, 3.0F, 17U};
    r.get<s::CharacterController>(e).speed = 9.0F;
    r.get<s::CharacterController>(e).character = 5U;
    r.get<s::AudioSource>(e).clip = "assets/audio/hum.wav";
    r.get<s::AudioSource>(e).sound = 42;
    r.get<s::AudioSource>(e).started = true;
    r.get<s::NetReplicated>(e).net_id = 99U;
    r.get<s::PatrolRoute>(e).points = {glm::vec3(1.0F), glm::vec3(2.0F)};
    r.get<s::BehaviorTree>(e).source = "{}";
    r.get<s::MeshSource>(e).mesh_key = "a.gltf#mesh0.prim0";

    const entt::entity copy = engine::editor::duplicate_entity(scene, e);
    ASSERT_TRUE(r.valid(copy));
    ASSERT_NE(copy, e);

    for_each_listed(engine::editor::SnapshotComponents{}, [&]<typename T>() {
        if constexpr (!std::is_same_v<T, s::Children>) {
            EXPECT_TRUE(r.all_of<T>(copy)) << "component not duplicated";
        }
    });
    EXPECT_EQ(r.get<s::Name>(copy).value, "crate copy");
    EXPECT_EQ(r.get<s::Transform>(copy).local[3], glm::vec4(1.0F, 2.0F, 3.0F, 1.0F));
    EXPECT_EQ(r.get<s::Collider>(copy).half_extents, glm::vec3(2.0F));
    EXPECT_EQ(r.get<s::RigidBody>(copy).motion, s::BodyMotion::kinematic);
    EXPECT_FLOAT_EQ(r.get<s::RigidBody>(copy).mass, 3.0F);
    EXPECT_FLOAT_EQ(r.get<s::CharacterController>(copy).speed, 9.0F);
    EXPECT_EQ(r.get<s::AudioSource>(copy).clip, "assets/audio/hum.wav");
    EXPECT_EQ(r.get<s::PatrolRoute>(copy).points.size(), 2U);
    EXPECT_EQ(r.get<s::BehaviorTree>(copy).source, "{}");
    EXPECT_EQ(r.get<s::MeshSource>(copy).mesh_key, "a.gltf#mesh0.prim0");

    // Session state is the copy's own.
    EXPECT_EQ(r.get<s::RigidBody>(copy).body, s::RigidBody{}.body);
    EXPECT_EQ(r.get<s::CharacterController>(copy).character, s::CharacterController{}.character);
    EXPECT_EQ(r.get<s::AudioSource>(copy).sound, engine::audio::invalid_sound);
    EXPECT_FALSE(r.get<s::AudioSource>(copy).started);
    EXPECT_EQ(r.get<s::NetReplicated>(copy).net_id, 0U);
    // The source is untouched.
    EXPECT_EQ(r.get<s::RigidBody>(e).body, 17U);
    EXPECT_EQ(r.get<s::AudioSource>(e).sound, engine::audio::SoundHandle{42});

    // A sibling under the same parent, without the source's children.
    EXPECT_EQ(r.get<s::Parent>(copy).entity, parent);
    const auto& kids = r.get<s::Children>(parent).entities;
    EXPECT_NE(std::find(kids.begin(), kids.end(), copy), kids.end());
    EXPECT_FALSE(r.all_of<s::Children>(copy));
}

// A10.1: gravity changed during Play (physics.set_gravity) reverts on Stop.
TEST(PlayModeSnapshot, RestoresThePhysicsSettings) {
    s::Scene scene;
    s::physics_settings_mut(scene.registry()).gravity = glm::vec3(0.0F, -3.0F, 0.0F);
    const auto snapshot = engine::editor::capture_scene(scene);
    s::physics_settings_mut(scene.registry()).gravity = glm::vec3(0.0F, 9.0F, 0.0F);
    engine::editor::restore_scene(scene, snapshot);
    EXPECT_EQ(s::physics_settings(scene.registry()).gravity, glm::vec3(0.0F, -3.0F, 0.0F));
}

} // namespace

#endif // ENGINE_EDITOR
