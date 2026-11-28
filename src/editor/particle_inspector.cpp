#include "particle_inspector.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "engine/scene/components.hpp"

namespace engine::editor {

namespace {

namespace r = renderer;

[[nodiscard]] float to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

// The picker shows sRGB (what artists pick); the component stores linear.
void color_srgb_alpha(const char* label, glm::vec4& linear) {
    glm::vec4 srgb(to_srgb(linear.r), to_srgb(linear.g), to_srgb(linear.b), linear.a);
    if (ImGui::ColorEdit4(label, &srgb.x)) {
        linear = glm::vec4(to_linear(srgb.r), to_linear(srgb.g), to_linear(srgb.b), srgb.a);
    }
}

template <typename E>
void combo(const char* label, E& value, const char* const* names, int count) {
    int i = static_cast<int>(value);
    if (ImGui::Combo(label, &i, names, count)) value = static_cast<E>(i);
}

void range(const char* label, glm::vec2& v, float speed, float lo, float hi, const char* fmt) {
    ImGui::DragFloatRange2(label, &v.x, &v.y, speed, lo, hi, fmt);
}

// Up to particle_curve_keys keys; `edit` draws one key's value.
template <typename Key, typename Edit>
void curve(const char* label, std::array<Key, r::particle_curve_keys>& keys, std::uint32_t& count,
           Edit&& edit) {
    if (!ImGui::TreeNode(label)) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(60.0F);
        ImGui::DragFloat("##t", &keys[i].t, 0.01F, 0.0F, 1.0F, "t %.2f");
        ImGui::SameLine();
        edit(keys[i]);
        ImGui::PopID();
    }
    if (count < r::particle_curve_keys && ImGui::SmallButton("+ key")) {
        keys[count] = keys[count - 1];
        keys[count].t = 1.0F;
        ++count;
    }
    if (count > 1) {
        ImGui::SameLine();
        if (ImGui::SmallButton("- key")) --count;
    }
    ImGui::TreePop();
}

} // namespace

void draw_particle_emitter(scene::ParticleEmitter& emitter) {
    r::ParticleEmitterSettings& s = emitter.settings;
    ImGui::Checkbox("enabled", &s.enabled);
    ImGui::SetItemTooltip("Off: no new particles; live ones finish their lifetime");
    int seed = static_cast<int>(s.seed & 0x7FFFFFFFU);
    if (ImGui::DragInt("seed", &seed, 1.0F, 0, 0x7FFFFFFF)) s.seed = static_cast<std::uint32_t>(seed);
    int max_particles = static_cast<int>(s.max_particles);
    if (ImGui::DragInt("max particles", &max_particles, 8.0F, 1,
                       static_cast<int>(r::particle_max_per_emitter))) {
        s.max_particles = static_cast<std::uint32_t>(max_particles);
    }
    ImGui::SetItemTooltip("This emitter's share of the global particle pool (changing it restarts it)");

    ImGui::SeparatorText("Emission");
    ImGui::DragFloat("rate", &s.rate, 1.0F, 0.0F, 100000.0F, "%.1f / s");
    int burst = static_cast<int>(s.burst_count);
    if (ImGui::DragInt("burst count", &burst, 1.0F, 0, static_cast<int>(r::particle_max_per_emitter))) {
        s.burst_count = static_cast<std::uint32_t>(burst);
    }
    ImGui::DragFloat("burst interval", &s.burst_interval, 0.05F, 0.0F, 3600.0F, "%.2f s");
    ImGui::SetItemTooltip("0: a single burst when the emitter starts");
    range("lifetime", s.lifetime, 0.05F, 0.01F, 600.0F, "%.2f s");

    ImGui::SeparatorText("Shape");
    static constexpr const char* shapes[] = {"point", "sphere", "box", "cone"};
    combo("shape", s.shape, shapes, 4);
    if (s.shape == r::ParticleShape::sphere || s.shape == r::ParticleShape::cone) {
        ImGui::DragFloat("radius", &s.radius, 0.01F, 0.0F, 1000.0F, "%.2f m");
    }
    if (s.shape == r::ParticleShape::box) {
        ImGui::DragFloat3("half extents", &s.box_half_extents.x, 0.01F, 0.0F, 1000.0F, "%.2f");
    }
    if (s.shape == r::ParticleShape::cone) {
        ImGui::SliderFloat("cone angle", &s.cone_angle_deg, 0.0F, 90.0F, "%.1f deg");
    } else if (s.shape != r::ParticleShape::sphere) {
        ImGui::SliderFloat("spread", &s.spread_deg, 0.0F, 180.0F, "%.1f deg");
    }
    if (s.shape != r::ParticleShape::sphere &&
        ImGui::DragFloat3("direction", &s.direction.x, 0.01F, -1.0F, 1.0F, "%.2f") &&
        glm::length(s.direction) > 1e-4F) {
        s.direction = glm::normalize(s.direction);
    }
    range("speed", s.speed, 0.05F, 0.0F, 1000.0F, "%.2f m/s");

    ImGui::SeparatorText("Forces");
    ImGui::DragFloat3("gravity", &s.gravity.x, 0.05F, -1000.0F, 1000.0F, "%.2f");
    ImGui::DragFloat("drag", &s.drag, 0.01F, 0.0F, 100.0F, "%.2f / s");

    ImGui::SeparatorText("Appearance");
    range("size", s.size, 0.01F, 0.001F, 1000.0F, "%.3f m");
    range("rotation", s.rotation_deg, 1.0F, -3600.0F, 3600.0F, "%.0f deg");
    range("spin", s.spin_deg, 1.0F, -3600.0F, 3600.0F, "%.0f deg/s");
    curve("size over life", s.size_curve, s.size_keys, [](r::ParticleSizeKey& k) {
        ImGui::DragFloat("##size", &k.size, 0.01F, 0.0F, 1000.0F, "x %.2f");
    });
    curve("colour over life", s.color_curve, s.color_keys,
          [](r::ParticleColorKey& k) { color_srgb_alpha("##color", k.color); });
    ImGui::DragFloat("emissive", &s.emissive, 0.05F, 0.0F, 1000.0F, "%.2f");
    ImGui::SetItemTooltip("Brightness multiplier of unlit particles (HDR)");
    ImGui::InputText("texture", &s.texture);
    ImGui::SetItemTooltip("Flipbook atlas under assets/ (empty: a soft disc)");
    int grid[2] = {static_cast<int>(s.flipbook_grid.x), static_cast<int>(s.flipbook_grid.y)};
    if (ImGui::DragInt2("flipbook", grid, 0.1F, 1, 64)) {
        s.flipbook_grid = glm::uvec2(static_cast<std::uint32_t>(grid[0]),
                                     static_cast<std::uint32_t>(grid[1]));
    }
    int frames = static_cast<int>(s.flipbook_frames);
    if (ImGui::DragInt("frames", &frames, 0.2F, 1, grid[0] * grid[1])) {
        s.flipbook_frames = static_cast<std::uint32_t>(frames);
    }
    ImGui::DragFloat("fps", &s.flipbook_fps, 0.1F, 0.0F, 240.0F, "%.1f");
    ImGui::SetItemTooltip("0: play the frames once over each particle's lifetime");

    ImGui::SeparatorText("Rendering");
    static constexpr const char* blends[] = {"additive", "alpha (OIT)"};
    combo("blend", s.blend, blends, 2);
    static constexpr const char* lighting[] = {"unlit", "lit"};
    combo("lighting", s.lighting, lighting, 2);
    static constexpr const char* spaces[] = {"world", "local"};
    combo("space", s.space, spaces, 2);
    ImGui::DragFloat("soft distance", &s.soft_distance, 0.01F, 0.0F, 100.0F, "%.2f m");
    ImGui::Checkbox("sort back to front", &s.sort);

    ImGui::SeparatorText("Collision");
    ImGui::Checkbox("collide with depth buffer", &s.collide);
    if (s.collide) {
        ImGui::SliderFloat("bounce", &s.bounce, 0.0F, 1.0F);
        ImGui::SliderFloat("friction", &s.friction, 0.0F, 1.0F);
    }
}

} // namespace engine::editor
