// Spline editing in the viewport and the spline inspector — see
// spline_gizmo.hpp.

#include "spline_gizmo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/spline/queries.hpp"
#include "ui/icons.hpp"
#include "ui/metrics.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

constexpr float k_pick_px = 9.0F;

struct View {
    glm::mat4 view_proj;
    glm::mat4 inv_view_proj;
    ImVec2    min;
    ImVec2    max;
};

[[nodiscard]] bool project(const View& v, const glm::vec3& world, ImVec2& out) {
    const glm::vec4 clip = v.view_proj * glm::vec4(world, 1.0F);
    if (clip.w <= 1e-4F) return false;
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    out.x = v.min.x + (ndc.x * 0.5F + 0.5F) * (v.max.x - v.min.x);
    out.y = v.min.y + (ndc.y * 0.5F + 0.5F) * (v.max.y - v.min.y);
    return true;
}

// The mouse ray hit on the horizontal plane y = plane_y (world).
[[nodiscard]] std::optional<glm::vec3> plane_hit(const View& v, const ImVec2& mouse, float plane_y) {
    const float w = std::max(v.max.x - v.min.x, 1.0F);
    const float h = std::max(v.max.y - v.min.y, 1.0F);
    const glm::vec2 ndc((mouse.x - v.min.x) / w * 2.0F - 1.0F, (mouse.y - v.min.y) / h * 2.0F - 1.0F);
    const glm::vec4 a = v.inv_view_proj * glm::vec4(ndc, 0.0F, 1.0F);
    const glm::vec4 b = v.inv_view_proj * glm::vec4(ndc, 1.0F, 1.0F);
    const glm::vec3 origin = glm::vec3(a) / a.w;
    const glm::vec3 dir = glm::vec3(b) / b.w - origin;
    if (std::abs(dir.y) < 1e-6F) return std::nullopt;
    const float t = (plane_y - origin.y) / dir.y;
    if (t < 0.0F) return std::nullopt;
    return origin + dir * t;
}

[[nodiscard]] float dist2(const ImVec2& a, const ImVec2& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] glm::mat4 world_of(const entt::registry& r, entt::entity e) {
    const auto* t = r.try_get<scene::Transform>(e);
    return t != nullptr ? t->world : glm::mat4(1.0F);
}

[[nodiscard]] bool is_anchor(const scene::Spline& s, std::size_t i) {
    return s.kind != scene::SplineKind::cubic_bezier || i % 3 == 0;
}

// The point under the mouse (anchors first), or -1.
[[nodiscard]] int hovered_point(const View& v, const scene::Spline& s, const glm::mat4& m, const ImVec2& mouse) {
    int best = -1;
    float best_d2 = k_pick_px * k_pick_px;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        ImVec2 p;
        if (!project(v, glm::vec3(m * glm::vec4(s.points[i].position, 1.0F)), p)) continue;
        const float d2 = dist2(p, mouse) * (is_anchor(s, i) ? 0.5F : 1.0F);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// The curve parameter under the mouse (screen distance <= k_pick_px).
[[nodiscard]] std::optional<float> hovered_param(const View& v, const spline::Curve& c, const ImVec2& mouse) {
    std::optional<float> best;
    float best_d2 = k_pick_px * k_pick_px;
    for (const spline::PolylinePoint& p : spline::flatten(c, 0.02F)) {
        ImVec2 s;
        if (!project(v, p.position, s)) continue;
        const float d2 = dist2(s, mouse);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = p.param;
        }
    }
    return best;
}

} // namespace

void spline_insert_point(scene::Spline& s, const spline::Curve& local, float param) {
    std::size_t seg = 0;
    float u = 0.0F;
    local.locate(param, seg, u);
    const spline::Segment& g = local.segments()[seg];
    const spline::CurveSample at = local.sample(param);
    const scene::SplinePoint p{at.position, at.width, at.height, g.tags};
    if (s.kind == scene::SplineKind::cubic_bezier) {
        // Split the segment's Bézier: anchor, two handles each side.
        glm::vec3 l[4];
        glm::vec3 r[4];
        spline::bezier_split(g.p, u, l, r);
        const std::size_t a = g.first_point;
        s.points[a + 1].position = l[1];
        s.points[a + 2].position = r[2];
        const auto it = s.points.begin() + static_cast<std::ptrdiff_t>(a + 2);
        s.points.insert(it, {{l[2], p.width, p.height, p.tags}, p, {r[1], p.width, p.height, p.tags}});
        return;
    }
    s.points.insert(s.points.begin() + static_cast<std::ptrdiff_t>(g.first_point + 1), p);
}

void spline_delete_point(scene::Spline& s, std::size_t i) {
    scene::Spline trial = s;
    if (s.kind == scene::SplineKind::cubic_bezier) {
        if (i % 3 != 0) return; // handles are not deleted
        const std::size_t n = trial.points.size();
        std::size_t first = i == 0 ? 0 : i - 1;
        if (!trial.closed && i == n - 1) first = i - 2;
        trial.points.erase(trial.points.begin() + static_cast<std::ptrdiff_t>(first),
                           trial.points.begin() + static_cast<std::ptrdiff_t>(std::min(first + 3, n)));
    } else {
        trial.points.erase(trial.points.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (spline::validate(trial)) s = std::move(trial); // never below the kind's minimum
}

namespace {

void draw_curve(ImDrawList* dl, const View& v, const spline::Curve& c, ImU32 color) {
    const std::vector<spline::PolylinePoint> pts = spline::flatten(c, 0.02F);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        ImVec2 a;
        ImVec2 b;
        if (project(v, pts[i - 1].position, a) && project(v, pts[i].position, b)) dl->AddLine(a, b, color, 2.0F);
    }
}

void draw_points(ImDrawList* dl, const View& v, const scene::Spline& s, const glm::mat4& m, int hot) {
    const ImU32 anchor = ui::to_u32(ui::tokens().accent);
    const ImU32 handle = ui::to_u32(ui::tokens().text_secondary);
    const ImU32 hot_col = ui::to_u32(ui::tokens().text);
    std::vector<ImVec2> screen(s.points.size(), ImVec2(-1.0e9F, -1.0e9F));
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        (void)project(v, glm::vec3(m * glm::vec4(s.points[i].position, 1.0F)), screen[i]);
    }
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const ImVec2 p = screen[i];
        if (p.x < -1.0e8F) continue;
        const ImU32 col = static_cast<int>(i) == hot ? hot_col : (is_anchor(s, i) ? anchor : handle);
        if (is_anchor(s, i)) {
            dl->AddCircleFilled(p, 5.0F, col);
        } else {
            // Bézier handle: a square tied to its anchor.
            const std::size_t a = i % 3 == 1 ? i - 1 : (i + 1) % s.points.size();
            if (screen[a].x > -1.0e8F) dl->AddLine(screen[a], p, handle, 1.0F);
            dl->AddRectFilled(ImVec2(p.x - 3.5F, p.y - 3.5F), ImVec2(p.x + 3.5F, p.y + 3.5F), col);
        }
    }
}

void finish_stroke(scene::Spline& s, const glm::mat4& m, SplineGizmoState& state) {
    auto fit = state.capture.end();
    if (!fit) {
        state.message = fit.error().message;
        return;
    }
    scene::Spline next = spline::transformed(fit->spline, glm::inverse(m));
    next.follow_ground = s.follow_ground;
    next.ground_offset = s.ground_offset;
    next.network = s.network;
    next.id = s.id;
    for (scene::SplinePoint& p : next.points) {
        p.width = s.points.empty() ? 1.0F : s.points.front().width;
    }
    s = std::move(next);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%zu points%s", s.points.size(), s.closed ? ", closed loop" : "");
    state.message = buf;
}

} // namespace

bool spline_gizmo(scene::Scene& scene, entt::entity selected, const glm::mat4& view_proj, const ImVec2& min,
                  const ImVec2& max, bool hovered, SplineGizmoState& state) {
    entt::registry& r = scene.registry();
    if (selected == entt::null || !r.valid(selected) || !r.all_of<scene::Spline>(selected)) {
        state.drag_point = -1;
        if (state.capture.active()) state.capture.cancel();
        return false;
    }
    scene::Spline& s = r.get<scene::Spline>(selected);
    const glm::mat4 m = world_of(r, selected);
    const View v{view_proj, glm::inverse(view_proj), min, max};
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool over_button = mouse.x >= state.button_min.x && mouse.x <= state.button_max.x &&
                             mouse.y >= state.button_min.y && mouse.y <= state.button_max.y;
    const float base_y = m[3].y;
    bool used = false;

    if (state.draw_mode) {
        if (state.capture.active()) {
            if (const auto hit = plane_hit(v, mouse, base_y)) state.capture.move(*hit, io.KeyShift);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) finish_stroke(s, m, state);
            used = true;
        } else if (hovered && !over_button && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (const auto hit = plane_hit(v, mouse, base_y)) {
                state.capture.begin(*hit);
                used = true;
            }
        }
    } else if (state.drag_point >= 0 && static_cast<std::size_t>(state.drag_point) < s.points.size()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (const auto hit = plane_hit(v, mouse, state.drag_plane_y)) {
                s.points[static_cast<std::size_t>(state.drag_point)].position =
                    glm::vec3(glm::inverse(m) * glm::vec4(*hit, 1.0F));
            }
        } else {
            state.drag_point = -1;
        }
        used = true;
    } else if (hovered && !over_button && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int hot = hovered_point(v, s, m, mouse);
        if (hot >= 0 && io.KeyAlt) {
            spline_delete_point(s, static_cast<std::size_t>(hot));
            used = true;
        } else if (hot >= 0) {
            state.drag_point = hot;
            state.drag_plane_y = glm::vec3(m * glm::vec4(s.points[static_cast<std::size_t>(hot)].position, 1.0F)).y;
            used = true;
        } else if (io.KeyCtrl) {
            if (auto local = spline::Curve::build(s)) {
                auto world = spline::Curve::build(spline::transformed(s, m));
                if (world) {
                    if (const auto param = hovered_param(v, *world, mouse)) {
                        spline_insert_point(s, *local, *param);
                        used = true;
                    }
                }
            }
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(min, max, true);
    if (auto world = spline::Curve::build(spline::transformed(s, m))) {
        draw_curve(dl, v, *world, ui::to_u32(ui::tokens().accent));
    }
    if (state.capture.active()) {
        const auto pts = state.capture.points();
        for (std::size_t i = 1; i < pts.size(); ++i) {
            ImVec2 a;
            ImVec2 b;
            if (project(v, pts[i - 1].position, a) && project(v, pts[i].position, b)) {
                dl->AddLine(a, b, ui::to_u32(ui::tokens().text), 1.5F);
            }
        }
    }
    const int hot = state.drag_point >= 0 ? state.drag_point : (hovered ? hovered_point(v, s, m, mouse) : -1);
    draw_points(dl, v, s, m, hot);
    dl->PopClipRect();
    return used;
}

void spline_gizmo_overlay(scene::Scene& scene, entt::entity selected, const ImVec2& min, const ImVec2& max,
                          SplineGizmoState& state) {
    const entt::registry& r = scene.registry();
    if (selected == entt::null || !r.valid(selected) || !r.all_of<scene::Spline>(selected)) {
        state.draw_mode = false;
        state.button_min = state.button_max = ImVec2(0, 0);
        return;
    }
    const float pad = ui::scaled(8.0F);
    const float line = ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorScreenPos(ImVec2(min.x + pad, max.y - pad - 2.0F * line));
    char label[64];
    std::snprintf(label, sizeof label, "%s %s###spline_draw", icon::route,
                  state.draw_mode ? "Drawing (Shift: straight)" : "Draw stroke");
    if (state.draw_mode) ImGui::PushStyleColor(ImGuiCol_Button, ui::to_u32(ui::tokens().accent));
    if (ImGui::SmallButton(label)) state.draw_mode = !state.draw_mode;
    if (state.draw_mode) ImGui::PopStyleColor();
    state.button_min = ImGui::GetItemRectMin();
    state.button_max = ImGui::GetItemRectMax();
    const char* hint = state.draw_mode ? "Drag on the ground to draw; the stroke replaces the spline"
                                       : "Drag points · Ctrl+click inserts · Alt+click deletes";
    ImGui::SetCursorScreenPos(ImVec2(min.x + pad, max.y - pad - line));
    ImGui::TextDisabled("%s%s%s", hint, state.message.empty() ? "" : " — ", state.message.c_str());
}

void draw_spline_inspector(const entt::registry& r, entt::entity e, scene::Spline& s) {
    static constexpr std::array k_kinds = {
        ui::EnumItem{static_cast<int>(scene::SplineKind::centripetal_catmull_rom), "Catmull-Rom",
                     "Smooth through every point (centripetal: no loops or cusps)"},
        ui::EnumItem{static_cast<int>(scene::SplineKind::cubic_bezier), "Bezier",
                     "Anchor, handle, handle, anchor ... (3k + 1 points)"},
        ui::EnumItem{static_cast<int>(scene::SplineKind::polyline), "Polyline", "Straight segments"},
    };
    (void)ui::prop_enum("Kind", s.kind, k_kinds);
    (void)ui::prop_bool("Closed", s.closed, {.tooltip = "Loop back to the first point"});
    (void)ui::prop_bool("Follow ground", s.follow_ground, {.tooltip = "Drape samples on the terrain"});
    if (s.follow_ground) {
        (void)ui::prop_float("Ground offset", s.ground_offset,
                             {.speed = 0.01F, .min = -100.0F, .max = 100.0F, .format = "%.2f m"});
    }
    (void)ui::prop_u32("Network", s.network, {.min = 0, .max = 1 << 30},
                       {.tooltip = "Splines with the same non-zero network join one curve network"});
    (void)ui::prop_u32("Id", s.id, {.min = 0, .max = 1 << 30},
                       {.tooltip = "Stable id within the network (0: the entity id)"});
    if (!s.points.empty()) {
        float width = s.points.front().width;
        if (ui::prop_float("Width (all)", width, {.speed = 0.05F, .min = 0.0F, .max = 1000.0F, .format = "%.2f m"})) {
            for (scene::SplinePoint& p : s.points) p.width = width;
        }
        float height = s.points.front().height;
        if (ui::prop_float("Height (all)", height, {.speed = 0.05F, .min = -1000.0F, .max = 1000.0F, .format = "%.2f m"})) {
            for (scene::SplinePoint& p : s.points) p.height = height;
        }
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%zu", s.points.size());
    ui::prop_value("Points", buf);
    if (const auto* cache = r.try_get<scene::SplineCache>(e)) {
        if (cache->valid) {
            std::snprintf(buf, sizeof buf, "%.2f m", static_cast<double>(cache->curve.length()));
            ui::prop_value("Length", buf);
        } else {
            ui::prop_note(cache->error.c_str(), ui::tokens().error);
        }
    }
}

} // namespace engine::editor
