#pragma once

// PlayMode — the Play/Stop state machine of the `game` executable (app
// layer), shared by the editor (Play button, sim.play / sim.stop) and the
// non-editor game (QUAT_SCENE / QUAT_PLAY runs start playing; sim.play /
// sim.stop work there too).
//
// Callers only request a state (`*request() = true`); apply() performs the
// transition at a fixed point of the frame: Play snapshots the scene, gives
// collider-only entities a static body and starts the world's play
// simulation (physics, AI, scripts); Stop ends it and restores the snapshot
// (exact entity ids). A Play that fails to start clears the request.
// While playing, remote scene.save / scene.load are refused (they read the
// requested state through request()).

#include <expected>
#include <functional>
#include <optional>

#include "engine/core/error.hpp"
#include "engine/scene/scene_snapshot.hpp"

namespace engine::scene {
class Scene;
}

namespace quat::app {

class PlayMode {
public:
    // What a transition starts and stops (the WorldRuntime's begin_play /
    // end_play in the game; fakes in tests).
    struct Hooks {
        std::function<std::expected<void, engine::core::Error>(engine::scene::Scene&)> begin;
        std::function<void()>                                                        end;
    };

    // The requested state, written by the UI, the remote bridge and startup.
    [[nodiscard]] bool* request() { return &requested_; }
    [[nodiscard]] bool requested() const { return requested_; }
    // A play session is running (the last apply() started one).
    [[nodiscard]] bool active() const { return active_; }
    // While playing: the edit scene as Play captured it (Stop restores it).
    // Null outside play. The device-loss recovery save writes it.
    [[nodiscard]] const engine::scene::SceneSnapshot* edit_snapshot() const {
        return active_ && snapshot_ ? &*snapshot_ : nullptr;
    }

    // Performs the requested transition, if any. Returns false when a Play
    // failed to start (the error is logged and the request cleared).
    bool apply(engine::scene::Scene& scene, const Hooks& hooks);

private:
    bool requested_ = false;
    bool active_ = false;
    std::optional<engine::scene::SceneSnapshot> snapshot_;
};

} // namespace quat::app
