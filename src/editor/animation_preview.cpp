#include "animation_preview.hpp"

#include <algorithm>
#include <vector>

#include <imgui.h>

#include "engine/animation/skeleton.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor {

namespace {

// Projects a world-space point into viewport-panel pixels. Returns false when
// the point is behind the camera.
[[nodiscard]] bool project(const glm::mat4& view_proj, const glm::vec3& world,
                           const float rect[4], ImVec2& out) {
    const glm::vec4 clip = view_proj * glm::vec4(world, 1.0F);
    if (clip.w <= 1e-4F) return false;
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    // The projection already carries the Vulkan Y flip, so NDC y grows
    // downward — matching screen coordinates directly.
    out.x = rect[0] + (ndc.x * 0.5F + 0.5F) * (rect[2] - rect[0]);
    out.y = rect[1] + (ndc.y * 0.5F + 0.5F) * (rect[3] - rect[1]);
    return true;
}

void draw_joint_overlay(const scene::SkinnedMesh& skinned, const glm::mat4& view_proj,
                        const float rect[4]) {
    if (!skinned.skeleton.valid() || !skinned.skeleton.is_loaded()) return;
    const animation::SkeletonAsset& skeleton = *skinned.skeleton;
    if (skeleton.joints.empty() ||
        skinned.joint_matrices.size() != skeleton.joints.size()) {
        return;
    }

    // joint_matrices are skinning matrices (root * world * inverse_bind), so
    // applying one to its joint's bind-pose world position yields the joint's
    // current world position.
    const std::vector<glm::mat4> bind_world = animation::compute_bind_world(skeleton);
    std::vector<glm::vec3> positions(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        const glm::vec3 bind_pos = glm::vec3(bind_world[i][3]);
        positions[i] = glm::vec3(skinned.joint_matrices[i] * glm::vec4(bind_pos, 1.0F));
    }

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(rect[0], rect[1]), ImVec2(rect[2], rect[3]), true);
    const ImU32 bone_color = IM_COL32(255, 196, 32, 220);
    const ImU32 joint_color = IM_COL32(255, 255, 255, 220);
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        ImVec2 p;
        if (!project(view_proj, positions[i], rect, p)) continue;
        draw->AddCircleFilled(p, 2.5F, joint_color);
        const int parent = skeleton.joints[i].parent;
        if (parent >= 0) {
            ImVec2 pp;
            if (project(view_proj, positions[static_cast<std::size_t>(parent)], rect, pp)) {
                draw->AddLine(pp, p, bone_color, 1.5F);
            }
        }
    }
    draw->PopClipRect();
}

} // namespace

void draw_animation_preview(scene::Scene& scene, entt::entity selected,
                            const glm::mat4& view_proj, const float viewport_rect[4],
                            AnimationPreviewState& state) {
    if (!ImGui::Begin("Animation")) {
        ImGui::End();
        return;
    }
    entt::registry& r = scene.registry();
    auto* animator =
        (selected != entt::null && r.valid(selected)) ? r.try_get<scene::Animator>(selected)
                                                      : nullptr;
    if (animator == nullptr) {
        ImGui::TextUnformatted("(select an entity with an Animator)");
        ImGui::End();
        return;
    }

    const float duration =
        (animator->clip.valid() && animator->clip.is_loaded()) ? animator->clip->duration : 0.0F;

    const bool playing = animator->speed != 0.0F;
    if (ImGui::Button(playing ? "Pause" : "Play")) {
        if (playing) {
            state.resume_speed = animator->speed;
            animator->speed = 0.0F;
        } else {
            animator->speed = state.resume_speed != 0.0F ? state.resume_speed : 1.0F;
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("loop", &animator->looping);
    ImGui::SameLine();
    ImGui::Checkbox("joints", &state.show_joints);

    float time = animator->time;
    if (ImGui::SliderFloat("time", &time, 0.0F, std::max(duration, 0.001F), "%.2fs")) {
        animator->time = time;
    }
    float speed = animator->speed;
    if (ImGui::SliderFloat("speed", &speed, -3.0F, 3.0F, "%.2fx")) {
        animator->speed = speed;
    }
    ImGui::Text("clip duration: %.2fs", duration);

    if (state.show_joints) {
        if (const auto* skinned = r.try_get<scene::SkinnedMesh>(selected)) {
            draw_joint_overlay(*skinned, view_proj, viewport_rect);
        }
    }
    ImGui::End();
}

} // namespace engine::editor
