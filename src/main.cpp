// quat-3d engine entry point.
//
// Process entry only: reads the process-level environment options and hands
// off to the application layer (src/app/application.hpp), which owns the
// window, device, scene, subsystems and the frame loop. Subsystems read their
// own environment variables (QUAT_GLTF / QUAT_PHYSICS / QUAT_CHARACTER /
// QUAT_TERRAIN in demo_scenes, QUAT_HOST / QUAT_JOIN / QUAT_NET_* in the net session,
// QUAT_REMOTE / QUAT_REMOTE_TOKEN in the remote bridge, QUAT_NO_AUDIO in the
// audio runtime).

#include <cstdlib>
#include <string>
#include <utility>

#include "app/application.hpp"
#include "app/remote_bridge.hpp"
#include "engine/core/env.hpp"
#include "engine/core/stderr_capture.hpp"

int main() {
    // A5: stderr -> engine log ring for debug.log (QUAT_REMOTE runs only).
    // Created first, destroyed last: it must outlive everything that logs.
    const engine::core::StderrCapture stderr_capture = quat::app::capture_stderr_for_remote();
    quat::app::AppConfig config;
    // QUAT_MAX_FRAMES, when set, auto-quits after that many presented frames.
    // Used for headless smoke tests so clean teardown can be verified.
    if (const char* env = std::getenv("QUAT_MAX_FRAMES"); env != nullptr) {
        config.max_frames = std::strtoull(env, nullptr, 10);
    }
    // QUAT_SCREENSHOT, combined with QUAT_MAX_FRAMES, writes the final presented
    // frame to the given path as binary PPM — headless visual verification.
    // Read as a path (UTF-16 environment on Windows), not via std::getenv.
    config.screenshot_path = engine::core::env_path("QUAT_SCREENSHOT");
    // QUAT_NO_UI=1 disables the editor shell (deterministic headless renders).
    config.editor_ui = std::getenv("QUAT_NO_UI") == nullptr;

    quat::app::Application app(std::move(config));
    return app.run();
}
