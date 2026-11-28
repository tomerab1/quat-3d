// quat-3d engine entry point (the editor build's `game`; quat-player has its
// own, src/player/main.cpp).
//
// Process entry only: reads the process-level environment options and hands
// off to the application layer (src/app/application.hpp), which owns the
// window, device, scene, subsystems and the frame loop. Subsystems read their
// own environment variables (QUAT_GLTF / QUAT_PHYSICS / QUAT_CHARACTER /
// QUAT_TERRAIN in demo_scenes, QUAT_HOST / QUAT_JOIN / QUAT_NET_* in the net session,
// QUAT_REMOTE / QUAT_REMOTE_TOKEN in the remote bridge, QUAT_NO_AUDIO in the
// audio runtime, QUAT_GAME in game_startup).

#include <cstdlib>
#include <utility>

#include "app/app_config.hpp"
#include "app/application.hpp"
#include "app/game_startup.hpp"
#include "app/remote_bridge.hpp"
#include "engine/core/env.hpp"
#include "engine/core/stderr_capture.hpp"

int main() {
    // A5: stderr -> engine log ring for debug.log and the editor Console.
    // Created first, destroyed last: it must outlive everything that logs.
    // Editor builds with the UI also capture it for the Console panel.
#ifdef ENGINE_EDITOR
    const bool editor_console = !engine::core::env_flag("QUAT_NO_UI");
#else
    const bool editor_console = false;
#endif
    const engine::core::StderrCapture stderr_capture =
        quat::app::capture_stderr_for_remote(editor_console);
    // QUAT_MAX_FRAMES, QUAT_SCREENSHOT, QUAT_NO_UI, QUAT_WINDOW_SIZE.
    quat::app::AppConfig config = quat::app::app_config_from_environment();
    // QUAT_GAME=<project dir>: edit (and Build & Export) a game project (P4).
    quat::app::open_game_from_environment(config);

    quat::app::Application app(std::move(config));
    return app.run();
}
