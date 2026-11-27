#include "audio_inspector.hpp"

#include <cstring>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/audio/audio_types.hpp"
#include "engine/scene/components.hpp"

namespace engine::editor {

namespace {

[[nodiscard]] const char* state_label(const scene::AudioSource& s) {
    if (s.sound == audio::invalid_sound) {
        // resolved is set once a load was attempted for the current clip.
        return s.resolved != 0 && !s.clip.empty() ? "load failed (see log)" : "not loaded";
    }
    switch (s.state) {
    case audio::Playback::playing:
        return "playing";
    case audio::Playback::paused:
        return "paused";
    case audio::Playback::stopped:
        break;
    }
    return "stopped";
}

} // namespace

void draw_audio_source(scene::AudioSource& source) {
    // Commit the path on Enter / focus loss only: every keystroke would
    // otherwise reload (and fail to load) a half-typed file name.
    char clip[1024]{};
    std::strncpy(clip, source.clip.c_str(), sizeof(clip) - 1);
    if (ImGui::InputText("clip", clip, sizeof(clip), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        source.clip = clip;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("wav / flac / mp3, relative to the project root (or absolute).\n"
                          "Music and ambience stream from disk; sfx and ui decode to memory.");
    }

    int bus = static_cast<int>(source.bus);
    const char* buses[audio::bus_count];
    for (std::size_t i = 0; i < audio::bus_count; ++i) buses[i] = audio::bus_names[i].data();
    if (ImGui::Combo("bus", &bus, buses, static_cast<int>(audio::bus_count))) {
        source.bus = static_cast<audio::AudioBus>(bus);
    }
    ImGui::SliderFloat("volume", &source.volume, 0.0F, 2.0F, "%.2f");
    ImGui::SliderFloat("pitch", &source.pitch, 0.25F, 4.0F, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("loop", &source.loop);
    ImGui::SameLine();
    ImGui::Checkbox("play on start", &source.play_on_start);
    ImGui::Checkbox("3D (spatial)", &source.spatial);
    if (source.spatial) {
        ImGui::DragFloat("min distance", &source.min_distance, 0.05F, 0.01F, 1000.0F, "%.2f m");
        ImGui::DragFloat("max distance", &source.max_distance, 0.5F, 0.01F, 100000.0F, "%.1f m");
        if (source.max_distance < source.min_distance) source.max_distance = source.min_distance;
        ImGui::SliderFloat("rolloff", &source.rolloff, 0.0F, 10.0F, "%.2f");
        ImGui::SliderFloat("doppler", &source.doppler, 0.0F, 2.0F, "%.2f");
    }

    ImGui::Separator();
    const float w = (ImGui::GetContentRegionAvail().x - 2.0F * ImGui::GetStyle().ItemSpacing.x) / 3.0F;
    if (ImGui::Button("Play", ImVec2(w, 0.0F))) source.command = scene::AudioCommand::play;
    ImGui::SameLine();
    const bool paused = source.state == audio::Playback::paused;
    if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(w, 0.0F))) {
        source.command = paused ? scene::AudioCommand::resume : scene::AudioCommand::pause;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(w, 0.0F))) source.command = scene::AudioCommand::stop;
    ImGui::TextDisabled("%s", state_label(source));
}

void draw_audio_listener(entt::registry& registry, entt::entity entity,
                         scene::AudioListener& listener) {
    if (ImGui::Checkbox("active", &listener.active) && listener.active) {
        for (auto [other, l] : registry.view<scene::AudioListener>().each()) {
            if (other != entity) l.active = false;
        }
    }
    ImGui::TextWrapped("The listener (the player's ears) follows this entity; without an "
                       "active one it follows the camera.");
}

} // namespace engine::editor
