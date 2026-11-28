// quat-player: the runtime player for games made with the engine (ROADMAP
// P4). The same engine and application layer as `game`, built without the
// editor: it boots game.json's start scene (src/app/game_startup.hpp) and
// runs it. Remote control is opt-in (QUAT_REMOTE) and only in development
// packages; shader hot reload is off. See docs/game-packaging.md.
//
// On Windows it is a GUI-subsystem program (no console window opens when a
// player double-clicks it); a terminal that redirects its output still gets
// the log through the inherited standard handles.

#include <cstdio>
#include <utility>

#include "app/app_config.hpp"
#include "app/application.hpp"
#include "app/game_startup.hpp"
#include "app/remote_bridge.hpp"
#include "engine/core/stderr_capture.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int run_player() {
    // stderr -> engine log ring for debug.log (QUAT_REMOTE runs only).
    // Created first, destroyed last: it must outlive everything that logs.
    const engine::core::StderrCapture stderr_capture = quat::app::capture_stderr_for_remote();
    quat::app::AppConfig config = quat::app::app_config_from_environment();
    if (auto booted = quat::app::boot_player(config); !booted) {
        std::fprintf(stderr, "[fatal] %s\n", booted.error().message.c_str());
        return 1;
    }
    quat::app::Application app(std::move(config));
    return app.run();
}

} // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*command_line*/,
                    int /*show*/) {
    return run_player();
}
#else
int main() {
    return run_player();
}
#endif
