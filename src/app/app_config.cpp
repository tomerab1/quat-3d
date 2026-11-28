#include "app/app_config.hpp"

#include <cstdio>
#include <cstdlib>

#include "engine/core/env.hpp"

namespace quat::app {

AppConfig app_config_from_environment() {
    AppConfig config;
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
    // QUAT_WINDOW_SIZE=<w>x<h> sets the initial window size (e.g. 1600x900 for
    // editor screenshots); malformed or out-of-range values keep the default.
    if (const char* env = std::getenv("QUAT_WINDOW_SIZE"); env != nullptr) {
        int w = 0;
        int h = 0;
        if (std::sscanf(env, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 16384 &&
            h <= 16384) {
            config.window_width = w;
            config.window_height = h;
        }
    }
    if (const char* env = std::getenv("QUAT_GPU_TIMEOUT_MS"); env != nullptr) {
        config.gpu_timeout_ns = parse_gpu_timeout_ms(env);
        if (!config.gpu_timeout_ns) {
            std::fprintf(stderr,
                         "[warn] QUAT_GPU_TIMEOUT_MS=%s ignored: expected milliseconds from "
                         "100 to 3600000\n",
                         env);
        }
    }
    return config;
}

std::optional<std::uint64_t> parse_gpu_timeout_ms(const char* text) {
    constexpr std::uint64_t k_min_ms = 100;
    constexpr std::uint64_t k_max_ms = 3'600'000;
    if (text == nullptr || *text == '\0') return std::nullopt;
    std::uint64_t ms = 0;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') return std::nullopt;
        ms = ms * 10 + static_cast<std::uint64_t>(*c - '0');
        if (ms > k_max_ms) return std::nullopt;
    }
    if (ms < k_min_ms) return std::nullopt;
    return ms * 1'000'000ULL;
}

} // namespace quat::app
