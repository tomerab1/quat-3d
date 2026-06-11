#include "physics_debug.hpp"

#include <array>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <imgui.h>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor {

namespace {

struct OverlayContext {
    ImDrawList*      draw = nullptr;
    const glm::mat4* view_proj = nullptr;
    const float*     rect = nullptr;
    ImU32            color = 0;
};

[[nodiscard]] bool project(const OverlayContext& ctx, const glm::vec3& world, ImVec2& out) {
    const glm::vec4 clip = *ctx.view_proj * glm::vec4(world, 1.0F);
    if (clip.w <= 1e-4F) return false;
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    out.x = ctx.rect[0] + (ndc.x * 0.5F + 0.5F) * (ctx.rect[2] - ctx.rect[0]);
    out.y = ctx.rect[1] + (ndc.y * 0.5F + 0.5F) * (ctx.rect[3] - ctx.rect[1]);
    return true;
}

void line(const OverlayContext& ctx, const glm::vec3& a, const glm::vec3& b) {
    ImVec2 pa;
    ImVec2 pb;
    if (project(ctx, a, pa) && project(ctx, b, pb)) {
        ctx.draw->AddLine(pa, pb, ctx.color, 1.0F);
    }
}

void draw_box(const OverlayContext& ctx, const glm::mat4& world, const glm::vec3& half,
              const glm::vec3& offset) {
    std::array<glm::vec3, 8> corners;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local((i & 1) != 0 ? half.x : -half.x, (i & 2) != 0 ? half.y : -half.y,
                              (i & 4) != 0 ? half.z : -half.z);
        corners[static_cast<std::size_t>(i)] =
            glm::vec3(world * glm::vec4(local + offset, 1.0F));
    }
    constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1},
                                                        {1, 3},
                                                        {3, 2},
                                                        {2, 0},
                                                        {4, 5},
                                                        {5, 7},
                                                        {7, 6},
                                                        {6, 4},
                                                        {0, 4},
                                                        {1, 5},
                                                        {2, 6},
                                                        {3, 7}}};
    for (const auto& e : edges) {
        line(ctx, corners[static_cast<std::size_t>(e[0])],
             corners[static_cast<std::size_t>(e[1])]);
    }
}

// Circle of `radius` around `centre`, in the plane spanned by axes `u`,`v`
// (local space, transformed by `world`).
void draw_circle(const OverlayContext& ctx, const glm::mat4& world, const glm::vec3& centre,
                 const glm::vec3& u, const glm::vec3& v, float radius) {
    constexpr int segments = 24;
    glm::vec3 prev{};
    for (int i = 0; i <= segments; ++i) {
        const float a =
            glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        const glm::vec3 local = centre + (u * std::cos(a) + v * std::sin(a)) * radius;
        const glm::vec3 point = glm::vec3(world * glm::vec4(local, 1.0F));
        if (i > 0) line(ctx, prev, point);
        prev = point;
    }
}

void draw_sphere(const OverlayContext& ctx, const glm::mat4& world, const glm::vec3& offset,
                 float radius) {
    draw_circle(ctx, world, offset, {1, 0, 0}, {0, 1, 0}, radius);
    draw_circle(ctx, world, offset, {1, 0, 0}, {0, 0, 1}, radius);
    draw_circle(ctx, world, offset, {0, 1, 0}, {0, 0, 1}, radius);
}

void draw_capsule(const OverlayContext& ctx, const glm::mat4& world, const glm::vec3& offset,
                  float half_height, float radius) {
    const glm::vec3 top = offset + glm::vec3(0, half_height, 0);
    const glm::vec3 bottom = offset - glm::vec3(0, half_height, 0);
    draw_circle(ctx, world, top, {1, 0, 0}, {0, 0, 1}, radius);
    draw_circle(ctx, world, bottom, {1, 0, 0}, {0, 0, 1}, radius);
    draw_sphere(ctx, world, top, radius);
    draw_sphere(ctx, world, bottom, radius);
    for (const glm::vec3 dir : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1),
                                glm::vec3(0, 0, -1)}) {
        line(ctx, glm::vec3(world * glm::vec4(top + dir * radius, 1.0F)),
             glm::vec3(world * glm::vec4(bottom + dir * radius, 1.0F)));
    }
}

} // namespace

void draw_physics_debug(scene::Scene& scene, const glm::mat4& view_proj,
                        const float viewport_rect[4], PhysicsDebugState& state,
                        bool* build_navmesh_request,
                        const std::vector<std::pair<glm::vec3, glm::vec3>>* nav_edges) {
    if (ImGui::Begin("Physics")) {
        ImGui::Checkbox("draw colliders", &state.enabled);
        ImGui::Checkbox("static", &state.show_static);
        ImGui::Checkbox("dynamic / kinematic", &state.show_dynamic);
        ImGui::Checkbox("sensors", &state.show_sensors);
        ImGui::Separator();
        if (build_navmesh_request != nullptr &&
            ImGui::Button("Build navmesh", ImVec2(-1.0F, 0.0F))) {
            *build_navmesh_request = true;
        }
        ImGui::Checkbox("show navmesh", &state.show_navmesh);
        if (nav_edges != nullptr && !nav_edges->empty()) {
            ImGui::Text("navmesh: %zu edges", nav_edges->size());
        }
    }
    ImGui::End();

    // Patrol routes: magenta polylines with point markers (always on — they
    // are authored content, not debug state).
    {
        OverlayContext route_ctx;
        route_ctx.draw = ImGui::GetForegroundDrawList();
        route_ctx.view_proj = &view_proj;
        route_ctx.rect = viewport_rect;
        route_ctx.color = IM_COL32(230, 80, 230, 220);
        route_ctx.draw->PushClipRect(ImVec2(viewport_rect[0], viewport_rect[1]),
                                     ImVec2(viewport_rect[2], viewport_rect[3]), true);
        for (auto [e, route] : scene.registry().view<const scene::PatrolRoute>().each()) {
            const auto& pts = route.points;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const glm::vec3 lift(0.0F, 0.15F, 0.0F);
                ImVec2 px;
                if (project(route_ctx, pts[i] + lift, px)) {
                    route_ctx.draw->AddCircleFilled(px, 4.0F, route_ctx.color);
                }
                if (i + 1 < pts.size()) {
                    line(route_ctx, pts[i] + lift, pts[i + 1] + lift);
                } else if (route.loop && pts.size() > 2) {
                    line(route_ctx, pts[i] + lift, pts[0] + lift);
                }
            }
        }
        route_ctx.draw->PopClipRect();
    }

    // Navmesh overlay draws independently of the collider toggle.
    if (state.show_navmesh && nav_edges != nullptr && !nav_edges->empty()) {
        OverlayContext nav_ctx;
        nav_ctx.draw = ImGui::GetForegroundDrawList();
        nav_ctx.view_proj = &view_proj;
        nav_ctx.rect = viewport_rect;
        nav_ctx.color = IM_COL32(60, 220, 220, 200); // cyan
        nav_ctx.draw->PushClipRect(ImVec2(viewport_rect[0], viewport_rect[1]),
                                   ImVec2(viewport_rect[2], viewport_rect[3]), true);
        for (const auto& [a, b] : *nav_edges) {
            line(nav_ctx, a + glm::vec3(0.0F, 0.05F, 0.0F),
                 b + glm::vec3(0.0F, 0.05F, 0.0F)); // lift off the ground
        }
        nav_ctx.draw->PopClipRect();
    }
    if (!state.enabled) return;

    OverlayContext ctx;
    ctx.draw = ImGui::GetForegroundDrawList();
    ctx.view_proj = &view_proj;
    ctx.rect = viewport_rect;
    ctx.draw->PushClipRect(ImVec2(viewport_rect[0], viewport_rect[1]),
                           ImVec2(viewport_rect[2], viewport_rect[3]), true);

    entt::registry& r = scene.registry();
    for (auto [e, transform, collider] :
         r.view<const scene::Transform, const scene::Collider>().each()) {
        const auto* body = r.try_get<scene::RigidBody>(e);
        const bool is_static =
            body == nullptr || body->motion == scene::BodyMotion::static_body;
        if (collider.is_sensor) {
            if (!state.show_sensors) continue;
            ctx.color = IM_COL32(255, 220, 40, 200); // sensors: yellow
        } else if (is_static) {
            if (!state.show_static) continue;
            ctx.color = IM_COL32(90, 220, 90, 200); // static: green
        } else {
            if (!state.show_dynamic) continue;
            ctx.color = IM_COL32(80, 170, 255, 200); // dynamic/kinematic: blue
        }

        switch (collider.shape) {
        case scene::ColliderShape::box:
            draw_box(ctx, transform.world, collider.half_extents, collider.offset);
            break;
        case scene::ColliderShape::sphere:
            draw_sphere(ctx, transform.world, collider.offset, collider.half_extents.x);
            break;
        case scene::ColliderShape::capsule:
            draw_capsule(ctx, transform.world, collider.offset, collider.half_extents.y,
                         collider.half_extents.x);
            break;
        }
    }
    ctx.draw->PopClipRect();
}

} // namespace engine::editor
