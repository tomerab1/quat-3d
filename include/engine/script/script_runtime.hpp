#pragma once

// ScriptRuntime — Lua gameplay scripts for one Scene (ROADMAP G7).
//
// Entities carry a scene::Script component naming a Lua file under
// assets/scripts/. While a play session runs (start() .. stop()), the
// runtime keeps one sandboxed Lua 5.4 state for the scene and one instance
// (`self` table) per scripted entity, and calls the script's callbacks:
//
//   on_start(self)                  once, before the instance's first update
//   on_fixed_update(self, dt)       at a fixed 60 Hz, before on_update
//   on_update(self, dt)             every frame (dt: simulation time)
//   on_trigger_enter(self, other)   a sensor this entity owns, or one it
//   on_trigger_exit(self, other)      entered, began / stopped overlapping
//   on_destroy(self)                the instance ends (entity destroyed,
//                                   Script removed, disabled or retargeted,
//                                   or the file hot-reloaded)
//
// A callback that raises an error, runs past its instruction budget or past
// its memory budget is stopped; the error (with the script line) goes to the
// engine log ring and the script's status, and that instance is disabled
// until its file changes. stop() drops the whole state, so the next session
// starts from scratch (the editor restores the scene snapshot on Stop).
//
// Hot reload: reload(path) (or poll_changes() with file watching on)
// recompiles a changed file; on success every instance of it is restarted
// (on_destroy, then a fresh self and on_start), on a compile or load error
// the previous version keeps running and the status shows the error.
//
// The Lua API is documented in docs/scripting.md. Main thread only.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/components.hpp"

namespace engine::physics {
class PhysicsWorld;
}
namespace engine::scene {
class DynamicMeshStore;
class Scene;
}
namespace engine::input {
class Actions;
struct DeviceState;
} // namespace engine::input

namespace engine::script {

using ScriptInput = input::DeviceState;

struct ScriptLimits {
    std::size_t   memory_bytes = 256U << 20;     // the scene's whole Lua heap
    std::size_t   call_memory_bytes = 64U << 20; // heap growth within one callback
    std::uint64_t call_instructions = 5'000'000; // Lua VM instructions per callback
};

// Per-script-file state, for the editor's Stats panel and script.list.
struct ScriptStatus {
    enum class State : std::uint8_t { ok, error };
    std::string   path; // relative to assets/scripts/
    State         state = State::ok;
    std::string   message;    // the last error (with its line), empty when ok
    std::uint32_t reloads = 0;   // successful hot reloads this session
    std::uint32_t instances = 0; // running instances
    std::uint32_t failed = 0;    // instances stopped by an error
};

// What scripts reach beyond the scene. Everything is optional; the
// corresponding calls fail with a clear message when it is missing.
struct ScriptServices {
    physics::PhysicsWorld* physics = nullptr; // the world being simulated
    const ScriptInput*     input = nullptr;
    // The action service (G5) behind input.axis / input.action: the
    // application updates it once per frame and the runtime takes one
    // simulation sample per update(). Without it the runtime evaluates the
    // default action map over `input` itself.
    input::Actions*        actions = nullptr;
    // Instantiates a glTF/GLB file (absolute path, already confined to the
    // project) and returns its root entities.
    std::function<std::expected<std::vector<entt::entity>, core::Error>(
        const std::filesystem::path& absolute)>
        spawn_model;
    std::filesystem::path project_root; // spawn_model paths are relative to it
    // Runtime-generated meshes (R23.2): the `geometry` table submits here.
    scene::DynamicMeshStore* dynamic_meshes = nullptr;
};

class ScriptRuntime {
public:
    struct Options {
        std::filesystem::path scripts_root; // absolute path of assets/scripts/
        ScriptLimits          limits;
        bool                  watch = false; // hot reload through a FileWatcher
    };

    explicit ScriptRuntime(Options options);
    ScriptRuntime();
    ~ScriptRuntime();
    ScriptRuntime(ScriptRuntime&&) noexcept;
    ScriptRuntime& operator=(ScriptRuntime&&) noexcept;
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    // ---- Session ------------------------------------------------------------
    // Starts a play session on `scene` with a fresh Lua state; instances are
    // created by the next update().
    [[nodiscard]] std::expected<void, core::Error> start(scene::Scene& scene);
    // Ends the session: drops every instance and the Lua state (no
    // on_destroy: the session's world is going away).
    void stop();
    [[nodiscard]] bool running() const;

    void set_services(ScriptServices services);

    // ---- Per frame (while running) ------------------------------------------
    // Ends stale instances, starts new ones, then runs on_fixed_update and
    // on_update; destroys requested during the phase happen at its end.
    void update(float dt);
    // Trigger callbacks for the overlap events of the physics step that just
    // ran (physics_system). Call after it, before the next update().
    void dispatch_triggers(const physics::PhysicsWorld& world);

    // ---- Files ----------------------------------------------------------------
    // Polls the file watcher (when on) and reloads changed scripts.
    void poll_changes();
    // Script `path` (relative to assets/scripts/) changed, appeared or was
    // removed: recompile it (and restart its instances) when running,
    // otherwise just re-check it.
    void reload(std::string_view path);

    [[nodiscard]] const std::vector<ScriptStatus>& statuses() const;

    // The properties script `path` declares (its `properties` table, with
    // their default values), for the inspector. Cached until the file
    // changes; loading runs the file's top level in a scratch state.
    [[nodiscard]] std::expected<std::vector<scene::ScriptProperty>, core::Error>
    declared_properties(std::string_view path);

    [[nodiscard]] const std::filesystem::path& scripts_root() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::script
