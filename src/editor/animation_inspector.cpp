#include "animation_inspector.hpp"

#include <cstdio>
#include <string>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/scene/animation_system.hpp"
#include "engine/scene/components.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

[[nodiscard]] std::string clip_label(const scene::Animator& a, std::size_t i) {
    const auto& h = a.clips[i];
    if (h.valid() && h.is_loaded() && !h->name.empty()) return h->name;
    return "#" + std::to_string(i);
}

[[nodiscard]] const char* joint_name(const animation::SkeletonAsset* skeleton, int joint) {
    if (skeleton == nullptr || joint < 0 ||
        static_cast<std::size_t>(joint) >= skeleton->joints.size()) {
        return "-";
    }
    return skeleton->joints[static_cast<std::size_t>(joint)].name.c_str();
}

} // namespace

void draw_animator(scene::Animator& animator) {
    if (!animator.clips.empty()) {
        std::string current = "(none)";
        for (std::size_t i = 0; i < animator.clips.size(); ++i) {
            if (animator.clips[i].id() == animator.clip.id()) current = clip_label(animator, i);
        }
        ui::prop_begin("Clip", "Switching uses the blend settings below", false, false);
        if (ImGui::BeginCombo("##clip", current.c_str())) {
            for (std::size_t i = 0; i < animator.clips.size(); ++i) {
                const bool selected = animator.clips[i].id() == animator.clip.id();
                if (ImGui::Selectable(clip_label(animator, i).c_str(), selected) && !selected) {
                    animator.clip = animator.clips[i];
                    animator.time = 0.0F;
                }
            }
            ImGui::EndCombo();
        }
        (void)ui::prop_end(false, false);
    }
    static constexpr ui::EnumItem modes[] = {
        {static_cast<int>(animation::BlendMode::inertialize), "Inertialize",
         "Decay the pose offset of the old clip (no double sampling)"},
        {static_cast<int>(animation::BlendMode::crossfade), "Crossfade",
         "Blend the old and new clips over the blend time"},
    };
    (void)ui::prop_enum("Blend", animator.blend_mode, modes,
                        {.reset = animation::BlendMode::inertialize});
    (void)ui::prop_float("Blend time", animator.blend_time,
                         {.speed = 0.01F, .min = 0.0F, .max = 2.0F, .format = "%.2f s"},
                         {.reset = 0.25F});
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(animator.time));
    ui::prop_value("Time", buf, nullptr, true);
    (void)ui::prop_float("Speed", animator.speed, {.speed = 0.05F, .min = -4.0F, .max = 4.0F},
                         {.reset = 1.0F});
    (void)ui::prop_bool("Looping", animator.looping, {.reset = true});
}

void draw_foot_ik(entt::registry& registry, entt::entity entity, scene::FootIK& k) {
    const char* auto_hint = "Joint name; empty = detected from common rig conventions";
    (void)ui::prop_text("Pelvis", k.pelvis, {.reset = std::string(), .tooltip = auto_hint}, true);
    (void)ui::prop_text("Left foot", k.left_foot, {.reset = std::string(), .tooltip = auto_hint},
                        true);
    (void)ui::prop_text("Right foot", k.right_foot,
                        {.reset = std::string(), .tooltip = auto_hint}, true);
    const scene::FootIK d{};
    (void)ui::prop_float("Ray up", k.ray_up,
                         {.speed = 0.01F, .min = 0.0F, .max = 10.0F, .format = "%.2f m"},
                         {.reset = d.ray_up, .tooltip = "Probe start above the ankle"});
    (void)ui::prop_float("Ray down", k.ray_down,
                         {.speed = 0.01F, .min = 0.0F, .max = 10.0F, .format = "%.2f m"},
                         {.reset = d.ray_down, .tooltip = "Probe reach below the ankle"});
    (void)ui::prop_float("Max adjust", k.max_adjust,
                         {.speed = 0.01F, .min = 0.0F, .max = 2.0F, .format = "%.2f m"},
                         {.reset = d.max_adjust, .tooltip = "Largest pelvis / foot offset"});
    (void)ui::prop_float("Max foot angle", k.max_foot_angle,
                         {.speed = 0.5F, .min = 0.0F, .max = 90.0F, .format = "%.0f°"},
                         {.reset = d.max_foot_angle, .tooltip = "Ground-alignment limit"});
    (void)ui::prop_float("Smoothing", k.smoothing,
                         {.speed = 0.005F, .min = 0.0F, .max = 2.0F, .format = "%.3f s"},
                         {.reset = d.smoothing, .tooltip = "Spring half-life"});

    const auto* rt = registry.try_get<scene::AnimationRuntime>(entity);
    const auto* skinned = registry.try_get<scene::SkinnedMesh>(entity);
    const animation::SkeletonAsset* skel =
        skinned != nullptr && skinned->skeleton.valid() && skinned->skeleton.is_loaded()
            ? &*skinned->skeleton
            : nullptr;
    if (rt == nullptr || skel == nullptr || rt->foot_rig_skeleton != skel) {
        ui::prop_value("Rig", "not evaluated yet");
        return;
    }
    if (!rt->foot_rig.valid()) {
        ui::prop_note("Rig not found: name the foot / pelvis joints.", ui::tokens().warning);
        return;
    }
    ui::prop_value("Pelvis joint", joint_name(skel, rt->foot_rig.pelvis), nullptr, true);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& leg = rt->foot_rig.legs[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s > %s > %s  %+.3f m%s", joint_name(skel, leg.upper),
                      joint_name(skel, leg.lower), joint_name(skel, leg.end),
                      static_cast<double>(rt->foot_result.foot_offset[i]),
                      rt->foot_result.grounded[i] ? "" : " (no ground)");
        ui::prop_value(i == 0 ? "Left leg" : "Right leg", buf, nullptr, true);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.3f m", static_cast<double>(rt->foot_result.pelvis_offset));
    ui::prop_value("Pelvis offset", buf, nullptr, true);
}

} // namespace engine::editor
