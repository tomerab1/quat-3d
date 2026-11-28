#include "app/audio_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include <entt/entity/registry.hpp>

#include "engine/scene/audio_system.hpp"

namespace quat::app {

AudioRuntime AudioRuntime::create(const std::filesystem::path& asset_root) {
    AudioRuntime out;
    engine::audio::AudioEngineConfig config;
    config.asset_root = asset_root;
    config.no_device = std::getenv("QUAT_NO_AUDIO") != nullptr;
    auto audio = engine::audio::AudioEngine::create(config);
    if (!audio && !config.no_device) {
        std::fprintf(stderr, "[audio] %s; continuing silently\n", audio.error().message.c_str());
        config.no_device = true;
        audio = engine::audio::AudioEngine::create(config);
    }
    if (!audio) {
        std::fprintf(stderr, "[audio] disabled: %s\n", audio.error().message.c_str());
        return out;
    }
    std::fprintf(stderr, "[audio] %s, %u Hz, %u channels\n",
                 audio->has_device() ? "device open" : "no device", audio->sample_rate(),
                 audio->channels());
    out.engine_ = std::move(*audio);
    return out;
}

void AudioRuntime::update(entt::registry& registry, const glm::mat4& view, bool live, float dt) {
    if (!engine_) return;
    if (live != was_live_) {
        engine_->stop_all(); // edit-mode previews end at Play; play sounds end at Stop
        was_live_ = live;
    }
    engine::scene::AudioSystemParams params;
    params.simulate = live;
    params.camera_world = glm::inverse(view);
    engine::scene::audio_system(registry, *engine_, dt, params);
}

} // namespace quat::app
