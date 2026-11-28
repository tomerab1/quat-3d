#pragma once

// WorldRuntime — the simulated world around the scene (app layer): the
// startup demo state, procedural terrain (async single-tile generation and
// seamless streaming, plus the tiles' play-mode height-field bodies),
// play-mode physics, behaviour trees and nav agents, gameplay scripts (G7),
// the navmesh, the network session (QUAT_HOST / QUAT_JOIN) and audio.
//
// Main-thread only. Not movable: the ground-height sampler and the remote
// bridge hold pointers into it.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "app/audio_runtime.hpp"
#include "app/demo_scenes.hpp"
#include "app/play_mode.hpp"
#include "app/grass_runtime.hpp"
#include "app/particle_runtime.hpp"
#include "engine/asset/mesh_asset.hpp" // Aabb
#include "engine/core/error.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/net/session.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/remote/world_commands.hpp" // TerrainStatus
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/dynamic_mesh.hpp"
#include "engine/scene/animation_system.hpp"
#include "engine/scene/navigation.hpp"
#include "engine/script/script_runtime.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/terrain/streamer.hpp"

namespace engine::asset {
class AssetManager;
}
namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi
namespace engine::scene {
class Scene;
}

namespace quat::app {

struct RemoteBridgeTargets;

// The navmesh and its bake requests. Bakes run in service_navmesh_request();
// the editor's Physics panel and play mode arm `request`.
struct NavmeshState {
    engine::nav::NavMesh mesh;     // baked on request from the Physics panel (13.1)
    bool                 request = false;
    bool                 auto_bake = false; // armed: bake once if Play has agents and no mesh
    std::string          status;            // last bake / invalidation, for the Physics panel
};

// Editor play mode's per-frame simulation input.
struct PlayStep {
    // Play looks through the scene's active Camera entity (menu-bar "Game
    // camera"); otherwise the trees perceive the fly camera at `fly_camera_pos`.
    bool      use_scene_camera = false;
    glm::vec3 fly_camera_pos{0.0F};
};

class WorldRuntime {
public:
    WorldRuntime();
    ~WorldRuntime();
    WorldRuntime(const WorldRuntime&) = delete;
    WorldRuntime& operator=(const WorldRuntime&) = delete;
    WorldRuntime(WorldRuntime&&) = delete;
    WorldRuntime& operator=(WorldRuntime&&) = delete;

    // ---- Setup -------------------------------------------------------------
    // Builds the startup scene (see demo_scenes.hpp). False on failure.
    // `start_scene` (a game's, P4) is loaded instead of a demo scene; its
    // render settings replace `render_settings`.
    [[nodiscard]] bool build_scene(engine::scene::Scene& scene,
                                   engine::asset::AssetManager& assets,
                                   engine::rhi::GpuAllocator& allocator,
                                   const engine::rhi::TransferContext& setup_transfer,
                                   const std::filesystem::path& project_root,
                                   engine::renderer::RenderSettings& render_settings,
                                   const std::optional<std::filesystem::path>& start_scene =
                                       std::nullopt);
    // The startup scene file's environment (QUAT_SCENE or a game's start
    // scene; empty otherwise).
    [[nodiscard]] const engine::scene::SceneEnvironment& startup_environment() const {
        return demo_.environment;
    }
    // Terrain pass + its upload context, the network session and the
    // QUAT_TERRAIN tile. After the renderer's environment exists.
    // `project_root` holds assets/ (particle flipbooks resolve against it).
    void init(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
              engine::rhi::PipelineCache& pipeline_cache, const std::string& shader_dir,
              const std::filesystem::path& project_root, engine::scene::Scene& scene);
    void init_audio(const std::filesystem::path& asset_root);
    // Gameplay scripts under <project_root>/assets/scripts. Hot reload is on
    // in editor builds (QUAT_SCRIPT_HOT_RELOAD=0 turns it off, =1 on in
    // non-editor builds). `services` (input, glTF spawning) are completed
    // with the play physics world when play starts.
    void init_scripts(const std::filesystem::path& project_root,
                      engine::script::ScriptServices services);
    [[nodiscard]] engine::script::ScriptRuntime* scripts() {
        return scripts_ ? &*scripts_ : nullptr;
    }
    // Points the remote bridge at the terrain, navmesh and demo state.
    void fill_remote_targets(RemoteBridgeTargets& targets);
    // Teardown, after the device went idle: waits for a running terrain job
    // and frees the upload context.
    void shutdown();

    // ---- Startup scene -----------------------------------------------------
    [[nodiscard]] bool physics_mode() const { return demo_.physics_mode; }
    [[nodiscard]] bool character_mode() const { return demo_.character_mode; }
    [[nodiscard]] bool from_gltf() const { return demo_.from_gltf; }
    // The scene came from a scene file (a game's start scene or QUAT_SCENE):
    // it brings its own lights and camera.
    [[nodiscard]] bool from_scene_file() const { return demo_.from_scene_file; }
    // Demo modes own their physics world (and so have no editor Play mode).
    [[nodiscard]] bool has_demo_physics() const { return demo_.physics_world.has_value(); }
    [[nodiscard]] entt::entity character_entity() const { return demo_.character_entity; }
    // The showcase's root entity (point lights hang under it); null otherwise.
    [[nodiscard]] entt::entity demo_root() const { return demo_.root; }
    // What the opening camera framing leaves out (the showcase's pedestal).
    [[nodiscard]] entt::entity framing_exclude() const { return demo_.pedestal; }
    // The loop drops the demo's entity references when a scene file replaces
    // the scene.
    void clear_demo_entities() {
        demo_.spin_entity = entt::null;
        demo_.root = entt::null;
        demo_.pedestal = entt::null;
    }
    // Vertical distance the demo content moved since the last call (settled
    // onto a freshly generated terrain tile); the view camera follows it.
    [[nodiscard]] float take_demo_shift() { return std::exchange(demo_shift_, 0.0F); }

    // ---- Per frame ---------------------------------------------------------
    // Character-mode input (heading-relative, normalised or zero).
    void drive_character(engine::scene::Scene& scene, const glm::vec3& wish, float dt);
    // Spins the demo object, runs networking and the simulation (demo physics,
    // or editor play mode when `play` is set), ticks the scene and publishes
    // replication. Networking runs on the real frame time `dt`; the
    // simulation systems and the scene tick on `sim_dt` (the remote debug
    // clock: 0 while paused, scaled by debug.time_scale), in slices of at
    // most PhysicsWorld::max_update_dt so physics advances it exactly.
    void simulate(engine::scene::Scene& scene, std::uint64_t frame_number, float dt,
                  float sim_dt, const PlayStep* play);
    void update_audio(engine::scene::Scene& scene, const glm::mat4& view, bool live, float dt);
    // Terrain upkeep around the rendered camera; uploads before the frame's
    // graph is built.
    void update_terrain(engine::scene::Scene& scene, const glm::vec3& camera_pos);
    // Bakes the navmesh if requested (blocking, logged).
    void service_navmesh_request(engine::scene::Scene& scene);

    // ---- Editor play mode --------------------------------------------------
    // Brings up the play physics world with the terrain as static height
    // fields, and arms the navmesh auto-bake.
    [[nodiscard]] std::expected<void, engine::core::Error> begin_play(engine::scene::Scene& scene);
    void end_play();
    // Performs `mode`'s requested Play/Stop transition with begin_play /
    // end_play (the editor's and the non-editor game's shared path). Emitters
    // start over on both transitions (one-shot bursts fire again in play;
    // play's particles vanish with play's scene). Returns true when the scene
    // switched between edit and play (the caller resets its temporal
    // history); false when nothing changed or a Play failed to start.
    bool apply_play(PlayMode& mode, engine::scene::Scene& scene);
    [[nodiscard]] bool play_physics_active() const { return play_physics_.has_value(); }
    // The physics world being simulated (play's, or a demo's), or null.
    [[nodiscard]] const engine::physics::PhysicsWorld* simulated_physics() const {
        if (play_physics_) return &*play_physics_;
        return demo_.physics_world ? &*demo_.physics_world : nullptr;
    }

    // Runtime-generated meshes whose static colliders the physics steps keep
    // in sync (R23.2). May be null (no dynamic mesh colliders).
    void set_dynamic_meshes(const engine::scene::DynamicMeshStore* store) { dynamic_meshes_ = store; }

    // ---- Navmesh / terrain -------------------------------------------------
    // A navmesh describes the geometry it was baked from: drop it when that
    // changes (scene load, terrain regeneration) and re-arm the auto-bake.
    void invalidate_navmesh(const char* why);
    [[nodiscard]] NavmeshState& navmesh() { return nav_; }

    // Authoritative ground height at world (x, z).
    [[nodiscard]] const engine::scene::GroundHeightFn& ground_height() const {
        return ground_height_at_;
    }
    // Terrain height above the anchor at anchor-relative (lx, lz); nullopt
    // where no tile is loaded, and everywhere while the single tile
    // regenerates (its old heightmap is stale).
    [[nodiscard]] std::optional<float> local_ground_height(float lx, float lz) const;
    // Tiles loaded / wanted, generation in flight, loaded extent (remote
    // terrain.status, issue #39).
    [[nodiscard]] engine::remote::TerrainStatus terrain_status() const;
    [[nodiscard]] bool terrain_drawable() const {
        return terrain_present_ && terrain_pass_.has_tiles();
    }
    [[nodiscard]] engine::renderer::TerrainPass& terrain_pass() { return terrain_pass_; }
    // GPU grass (R9.3); null before init().
    [[nodiscard]] GrassRuntime* grass() { return grass_ ? &*grass_ : nullptr; }
    // GPU particles (R10.3); null before init().
    [[nodiscard]] ParticleRuntime* particles() { return particles_ ? &*particles_ : nullptr; }
    // Restarts every particle emitter (editor Play / Stop).
    void reset_particles() {
        if (particles_) particles_->reset_all();
    }
    [[nodiscard]] const glm::vec3& terrain_anchor() const { return terrain_anchor_; }
    // World-space extent the terrain covers for a camera at `eye`: the
    // streaming window around it, or the single tile. nullopt without terrain.
    // The view camera's far plane reaches it (issue #37).
    [[nodiscard]] std::optional<engine::asset::Aabb> terrain_view_bounds(const glm::vec3& eye) const;
    [[nodiscard]] float terrain_snowline() const { return terrain_snowline_; }

private:
    // One slice of simulate(): physics, characters, behaviour trees and
    // navigation advanced by `sim_dt` (at most PhysicsWorld::max_update_dt).
    void simulate_slice(engine::scene::Scene& scene, float sim_dt, const PlayStep* play);

    // Issue #7: sets the showcase on the terrain — its root at the top of a
    // pedestal over the highest ground under the plate (+ room for grass),
    // the pedestal reaching below the lowest — so it neither floats nor is
    // pierced. Called whenever the single terrain tile arrives.
    void settle_demo_on_terrain(engine::scene::Scene& scene);

    // Cancels the single-tile job: GPU tiles are abandoned (their future
    // resolves at once, invalid) and a CPU job stops at its next step.
    void abandon_terrain_job();

    std::optional<engine::physics::PhysicsWorld> play_physics_; // editor play session's world
    DemoScene                                    demo_;
    float                                        demo_shift_ = 0.0F; // see take_demo_shift

    // Terrain (Phase 12): the pass owns the heightmap texture + LOD index
    // buffers; uploads go through a transfer pool that outlives setup.
    VkDevice                                 device_ = VK_NULL_HANDLE;
    VkCommandPool                            terrain_pool_ = VK_NULL_HANDLE;
    engine::rhi::TransferContext             terrain_transfer_{};
    engine::renderer::TerrainPass            terrain_pass_;
    std::optional<GrassRuntime>              grass_; // samples terrain_pass_'s tiles
    std::optional<ParticleRuntime>           particles_; // uploads flipbooks via terrain_transfer_
    std::future<engine::terrain::Heightmap>  terrain_job_;
    engine::terrain::CancelToken             terrain_cancel_; // stops terrain_job_ on the CPU
    engine::terrain::Heightmap               terrain_heightmap_; // CPU copy retained for physics (12.3)
    std::optional<engine::terrain::TerrainStreamer> terrain_streamer_; // 12.4
    std::string terrain_budget_error_; // last over-budget configuration reported (#67)
    // Terrain placement, written by the per-frame upkeep and read at the top
    // of the next frame by the nav ground snap (the anchor moves rarely).
    glm::vec3 terrain_anchor_{0.0F};
    float     terrain_snowline_ = 110.0F;
    bool      terrain_present_ = false;
    bool      terrain_stream_mode_ = false;

    NavmeshState nav_;

    // Networking (Phase 14; S1 B1-B5): QUAT_HOST=<port> hosts this scene,
    // QUAT_JOIN=<ip:port> joins a host running the same scene. See
    // engine/net/session.hpp for the per-frame protocol and
    // engine/net/session_config.hpp for the QUAT_NET_* host options.
    engine::net::NetSession net_;
    std::map<glm::ivec2, std::uint32_t, engine::terrain::TileCoordLess>
        terrain_bodies_; // streamed tiles' play-mode physics bodies

    engine::scene::GroundHeightFn  ground_height_at_;
    const engine::scene::DynamicMeshStore* dynamic_meshes_ = nullptr;
    engine::scene::FootGroundProbe foot_probe_;
    std::vector<std::uint32_t>     probe_owners_; // foot_probe_ scratch: owners to skip
    std::optional<AudioRuntime>   audio_;
    std::optional<engine::script::ScriptRuntime> scripts_;
    engine::script::ScriptServices               script_services_;
};

} // namespace quat::app
