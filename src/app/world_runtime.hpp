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
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "app/audio_runtime.hpp"
#include "app/demo_scenes.hpp"
#include "app/play_mode.hpp"
#include "app/grass_runtime.hpp"
#include "engine/core/error.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/net/session.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/gpu_allocator.hpp"
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
    [[nodiscard]] bool build_scene(engine::scene::Scene& scene,
                                   engine::asset::AssetManager& assets,
                                   engine::rhi::GpuAllocator& allocator,
                                   const engine::rhi::TransferContext& setup_transfer);
    // Terrain pass + its upload context, the network session and the
    // QUAT_TERRAIN tile. After the renderer's environment exists.
    void init(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
              engine::rhi::PipelineCache& pipeline_cache, const std::string& shader_dir,
              engine::scene::Scene& scene);
    void init_audio(const std::string& asset_root);
    // Gameplay scripts under <project_root>/assets/scripts. Hot reload is on
    // in editor builds (QUAT_SCRIPT_HOT_RELOAD=0 turns it off, =1 on in
    // non-editor builds). `services` (input, glTF spawning) are completed
    // with the play physics world when play starts.
    void init_scripts(const std::string& project_root, engine::script::ScriptServices services);
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
    [[nodiscard]] bool from_scene_file() const { return demo_.from_scene_file; }
    // Demo modes own their physics world (and so have no editor Play mode).
    [[nodiscard]] bool has_demo_physics() const { return demo_.physics_world.has_value(); }
    [[nodiscard]] entt::entity character_entity() const { return demo_.character_entity; }
    // The loop drops this when a scene file replaces the scene.
    void clear_spin_entity() { demo_.spin_entity = entt::null; }

    // ---- Per frame ---------------------------------------------------------
    // Character-mode input (heading-relative, normalised or zero).
    void drive_character(engine::scene::Scene& scene, const glm::vec3& wish, float dt);
    // Spins the demo object, runs networking and the simulation (demo physics,
    // or editor play mode when `play` is set), ticks the scene and publishes
    // replication. Networking runs on the real frame time `dt`; the
    // simulation systems and the scene tick on `sim_dt` (the remote debug
    // clock: 0 while paused, scaled by debug.time_scale).
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
    // end_play (the editor's and the non-editor game's shared path).
    bool apply_play(PlayMode& mode, engine::scene::Scene& scene);
    [[nodiscard]] bool play_physics_active() const { return play_physics_.has_value(); }

    // ---- Navmesh / terrain -------------------------------------------------
    // A navmesh describes the geometry it was baked from: drop it when that
    // changes (scene load, terrain regeneration) and re-arm the auto-bake.
    void invalidate_navmesh(const char* why);
    [[nodiscard]] NavmeshState& navmesh() { return nav_; }

    // Authoritative ground height at world (x, z).
    [[nodiscard]] const engine::scene::GroundHeightFn& ground_height() const {
        return ground_height_at_;
    }
    [[nodiscard]] bool terrain_drawable() const {
        return terrain_present_ && terrain_pass_.has_tiles();
    }
    [[nodiscard]] engine::renderer::TerrainPass& terrain_pass() { return terrain_pass_; }
    // GPU grass (R9.3); null before init().
    [[nodiscard]] GrassRuntime* grass() { return grass_ ? &*grass_ : nullptr; }
    [[nodiscard]] const glm::vec3& terrain_anchor() const { return terrain_anchor_; }
    [[nodiscard]] float terrain_snowline() const { return terrain_snowline_; }

private:
    // Cancels the single-tile job: GPU tiles are abandoned (their future
    // resolves at once, invalid) and a CPU job stops at its next step.
    void abandon_terrain_job();

    std::optional<engine::physics::PhysicsWorld> play_physics_; // editor play session's world
    DemoScene                                    demo_;

    // Terrain (Phase 12): the pass owns the heightmap texture + LOD index
    // buffers; uploads go through a transfer pool that outlives setup.
    VkDevice                                 device_ = VK_NULL_HANDLE;
    VkCommandPool                            terrain_pool_ = VK_NULL_HANDLE;
    engine::rhi::TransferContext             terrain_transfer_{};
    engine::renderer::TerrainPass            terrain_pass_;
    std::optional<GrassRuntime>              grass_; // samples terrain_pass_'s tiles
    std::future<engine::terrain::Heightmap>  terrain_job_;
    engine::terrain::CancelToken             terrain_cancel_; // stops terrain_job_ on the CPU
    engine::terrain::Heightmap               terrain_heightmap_; // CPU copy retained for physics (12.3)
    std::optional<engine::terrain::TerrainStreamer> terrain_streamer_; // 12.4
    // Terrain placement, written by the per-frame upkeep and read at the top
    // of the next frame by the nav ground snap (the anchor moves rarely).
    glm::vec3 terrain_anchor_{0.0F};
    float     terrain_snowline_ = 110.0F;
    bool      terrain_present_ = false;
    bool      terrain_stream_mode_ = false;

    NavmeshState nav_;

    // Networking (Phase 14; S1 B1-B5): QUAT_HOST=<port> hosts this scene,
    // QUAT_JOIN=<ip:port> joins a host running the same scene. See
    // engine/net/session.hpp for the per-frame protocol.
    engine::net::NetSession net_;
    std::map<glm::ivec2, std::uint32_t, engine::terrain::TileCoordLess>
        terrain_bodies_; // streamed tiles' play-mode physics bodies

    engine::scene::GroundHeightFn  ground_height_at_;
    engine::scene::FootGroundProbe foot_probe_;
    std::vector<std::uint32_t>     probe_owners_; // foot_probe_ scratch: owners to skip
    std::optional<AudioRuntime>   audio_;
    std::optional<engine::script::ScriptRuntime> scripts_;
    engine::script::ScriptServices               script_services_;
};

} // namespace quat::app
