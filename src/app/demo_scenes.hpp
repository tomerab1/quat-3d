#pragma once

// Demo / startup scenes for the `game` executable (app layer, not engine).
//
// A game's start scene (quat-player, P4) wins; otherwise selected by
// environment variables:
//   QUAT_GLTF=<file>   instantiate a glTF scene graph
//   QUAT_PHYSICS=1     a pile of boxes dropped on a floor (owns a physics world)
//   QUAT_CHARACTER=1   a WASD capsule character on a floor (owns a physics world)
//   (none)             the material showcase
// plus QUAT_TERRAIN=1 for a procedural terrain tile under any of them.

#include <expected>
#include <filesystem>
#include <optional>

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/physics/physics_world.hpp"

namespace engine::asset {
class AssetManager;
}
namespace engine::renderer {
struct RenderSettings;
}
namespace engine::rhi {
class GpuAllocator;
struct TransferContext;
} // namespace engine::rhi
namespace engine::scene {
class Scene;
}

namespace quat::app {

// What the frame loop needs to know about the scene it was handed.
struct DemoScene {
    bool physics_mode = false;   // QUAT_PHYSICS: physics_world steps rigid bodies
    bool character_mode = false; // QUAT_CHARACTER: physics_world steps the character
    bool from_gltf = false;      // QUAT_GLTF: show the model under its own lighting
    bool from_scene_file = false; // a start scene: no default lights / camera added
    std::optional<engine::physics::PhysicsWorld> physics_world;
    entt::entity character_entity = entt::null;
    // An object the loop slowly spins (the demo cube / glass sphere).
    entt::entity spin_entity = entt::null;
    glm::vec3    spin_pos{0.0F};
    float        spin_scale = 1.0F;
};

// Build the startup scene into `scene` (meshes/materials uploaded through
// `transfer` and cached in `assets`): `start_scene` when given (its render
// settings replace `*render_settings`), else a demo scene.
[[nodiscard]] std::expected<DemoScene, engine::core::Error>
build_demo_scene(engine::scene::Scene& scene, engine::asset::AssetManager& assets,
                 engine::rhi::GpuAllocator& allocator,
                 const engine::rhi::TransferContext& setup_transfer,
                 const std::optional<std::filesystem::path>& start_scene = std::nullopt,
                 engine::renderer::RenderSettings* render_settings = nullptr);

// The sun, plus two coloured point lights unless the scene came from a glTF
// file (a model is shown under just its own lighting setup).
void add_default_lighting(engine::scene::Scene& scene, bool fill_lights);

// QUAT_TERRAIN=1: a procedural terrain tile sunk under the scene.
void add_env_terrain(engine::scene::Scene& scene);

} // namespace quat::app
