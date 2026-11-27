#include "animation_inspector.hpp"

#include <string>

#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "engine/scene/animation_system.hpp"
#include "engine/scene/components.hpp"

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
        if (ImGui::BeginCombo("clip", current.c_str())) {
            for (std::size_t i = 0; i < animator.clips.size(); ++i) {
                const bool selected = animator.clips[i].id() == animator.clip.id();
                if (ImGui::Selectable(clip_label(animator, i).c_str(), selected) && !selected) {
                    animator.clip = animator.clips[i];
                    animator.time = 0.0F;
                }
            }
            ImGui::EndCombo();
        }
    }
    const char* modes[] = {"inertialize", "crossfade"};
    int mode = static_cast<int>(animator.blend_mode);
    if (ImGui::Combo("blend", &mode, modes, 2)) {
        animator.blend_mode = static_cast<animation::BlendMode>(mode);
    }
    ImGui::DragFloat("blend time", &animator.blend_time, 0.01F, 0.0F, 2.0F, "%.2fs");
    ImGui::Text("time: %.2fs", animator.time);
    ImGui::DragFloat("speed", &animator.speed, 0.05F, -4.0F, 4.0F);
    ImGui::Checkbox("looping", &animator.looping);
}

void draw_foot_ik(entt::registry& registry, entt::entity entity, scene::FootIK& k) {
    ImGui::Checkbox("enabled", &k.enabled);
    ImGui::InputTextWithHint("pelvis", "auto", &k.pelvis);
    ImGui::InputTextWithHint("left foot", "auto", &k.left_foot);
    ImGui::InputTextWithHint("right foot", "auto", &k.right_foot);
    ImGui::DragFloat("ray up", &k.ray_up, 0.01F, 0.0F, 10.0F, "%.2f m");
    ImGui::DragFloat("ray down", &k.ray_down, 0.01F, 0.0F, 10.0F, "%.2f m");
    ImGui::DragFloat("max adjust", &k.max_adjust, 0.01F, 0.0F, 2.0F, "%.2f m");
    ImGui::DragFloat("max foot angle", &k.max_foot_angle, 0.5F, 0.0F, 90.0F, "%.0f deg");
    ImGui::DragFloat("smoothing", &k.smoothing, 0.005F, 0.0F, 2.0F, "%.3f s");

    const auto* rt = registry.try_get<scene::AnimationRuntime>(entity);
    const auto* skinned = registry.try_get<scene::SkinnedMesh>(entity);
    const animation::SkeletonAsset* skel =
        skinned != nullptr && skinned->skeleton.valid() && skinned->skeleton.is_loaded()
            ? &*skinned->skeleton
            : nullptr;
    if (rt == nullptr || skel == nullptr || rt->foot_rig_skeleton != skel) {
        ImGui::TextDisabled("(not evaluated yet)");
        return;
    }
    if (!rt->foot_rig.valid()) {
        ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.2F, 1.0F),
                           "rig not found: name the foot / pelvis joints");
        return;
    }
    ImGui::TextDisabled("pelvis %s", joint_name(skel, rt->foot_rig.pelvis));
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& leg = rt->foot_rig.legs[i];
        ImGui::TextDisabled("%s: %s > %s > %s  %+.3f m%s", i == 0 ? "left" : "right",
                            joint_name(skel, leg.upper), joint_name(skel, leg.lower),
                            joint_name(skel, leg.end), rt->foot_result.foot_offset[i],
                            rt->foot_result.grounded[i] ? "" : " (no ground)");
    }
    ImGui::TextDisabled("pelvis offset %+.3f m", rt->foot_result.pelvis_offset);
}

} // namespace engine::editor
