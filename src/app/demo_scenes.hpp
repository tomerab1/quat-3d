#pragma once

// Demo / startup scenes for the `game` executable (app layer, not engine).
//
// A game's start scene (quat-player, P4) wins; otherwise selected by
// environment variables:
//   QUAT_SCENE=<file>  load a scene file (project-relative unless absolute),
//                      e.g. a scripted game; it brings its own lights
//   QUAT_DEMO=<name>   a showcase scene file from assets/scenes/ (k_demo_scenes)
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
#include "engine/scene/scene_io.hpp" // SceneEnvironment

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
    bool from_scene_file = false; // a start scene or QUAT_SCENE: no default lights / camera added
    std::optional<engine::physics::PhysicsWorld> physics_world;
    entt::entity character_entity = entt::null;
    // An object the loop slowly spins (the demo cube / glass sphere).
    entt::entity spin_entity = entt::null;
    glm::vec3    spin_pos{0.0F};
    float        spin_scale = 1.0F;
    // The showcase's content hangs under `root` (moved as a whole, e.g. onto
    // terrain). `pedestal` is its ground plate: a box whose top is at the
    // root's origin, `footprint_half` metres to each side in X and Z. It is
    // left out of the opening camera framing (the content is what to see).
    entt::entity root = entt::null;
    entt::entity pedestal = entt::null;
    glm::vec2    footprint_half{0.0F};
    // QUAT_SCENE: the file's scene-level environment (its HDRI, R21).
    engine::scene::SceneEnvironment environment;
};

// QUAT_DEMO scenes: a name and its scene file (relative to the project root).
// The file's render_settings block (shadow coverage, clouds, ...) applies.
struct DemoSceneFile {
    const char* name;
    const char* path;
};
inline constexpr DemoSceneFile k_demo_scenes[] = {
    {"mountain", "assets/scenes/mountain_forest.json"},
};

// Half the height of the pedestal mesh (a box) as built; its Transform scales
// it to the depth it needs.
inline constexpr float k_pedestal_mesh_half_height = 0.25F;

// Build the startup scene into `scene` (meshes/materials uploaded through
// `transfer` and cached in `assets`): `start_scene` when given (its render
// settings replace `render_settings`), else QUAT_SCENE (a relative path
// resolves against `project_root`), else a QUAT_DEMO scene file (also under
// `project_root`; sets `render_settings` too), else a demo scene.
[[nodiscard]] std::expected<DemoScene, engine::core::Error>
build_demo_scene(engine::scene::Scene& scene, engine::asset::AssetManager& assets,
                 engine::rhi::GpuAllocator& allocator,
                 const engine::rhi::TransferContext& setup_transfer,
                 const std::filesystem::path& project_root,
                 engine::renderer::RenderSettings& render_settings,
                 const std::optional<std::filesystem::path>& start_scene = std::nullopt);

// The sun, plus two coloured point lights unless the scene came from a glTF
// file (a model is shown under just its own lighting setup). The point lights
// hang under `root` when given, so they move with the demo content. A scene
// that brings its own sun (a scene file) keeps its lighting as authored.
void add_default_lighting(engine::scene::Scene& scene, bool fill_lights,
                          entt::entity root = entt::null);

// QUAT_TERRAIN=1: a procedural terrain tile sunk under the scene.
void add_env_terrain(engine::scene::Scene& scene);

// QUAT_TIME=<ISO-8601 date-time>: a world clock (R22.1) that drives the sun,
// with QUAT_LATLON=<lat>,<lon> (degrees; default Paris) and
// QUAT_TIME_SCALE=<clock seconds per second> (default 1; 0 freezes it, as
// the CI sunrise / noon / sunset screenshots do). Invalid values are
// reported on stderr and ignored.
void add_env_world_clock(engine::scene::Scene& scene);

// QUAT_SEASON=<spring|summer|autumn|winter|year phase>: a held season (R25.1)
// that drives the world clock's date (combine with QUAT_TIME for the time of
// day and place) and the weather, with QUAT_SEASON_SEED=<n> for the weather
// sequence; five simulated minutes of weather are lived through first, so
// snow and wet ground show. Invalid values are reported and ignored.
void add_env_season(engine::scene::Scene& scene);

} // namespace quat::app
