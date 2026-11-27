// Status bar (ROADMAP E8.2): simulation state, selection, entity count, the
// newest log line (click opens the Console), remote / MCP connection and
// frame timing.

#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // BeginViewportSideBar

#include "../panels/console.hpp"
#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor::shell {

namespace {

// A rounded "pill" with an icon and a label, in `fg` on a faint `fg` fill.
void pill(const char* glyph, const char* text, ui::Rgba fg) {
    const std::string s = ui::label(glyph, text);
    const ImVec2 size = ImGui::CalcTextSize(s.c_str());
    const float pad_x = ui::scaled(ui::Spacing::xs);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x + pad_x * 2.0F, p.y + h),
                      ui::to_u32(ui::with_alpha(fg, 0.16F)), h * 0.5F);
    dl->AddText(ImVec2(p.x + pad_x, p.y + (h - size.y) * 0.5F), ui::to_u32(fg), s.c_str());
    ImGui::Dummy(ImVec2(size.x + pad_x * 2.0F, h));
}

void separator_dot() {
    ImGui::SameLine(0.0F, ui::scaled(ui::Spacing::sm));
}

[[nodiscard]] std::string entity_name(const entt::registry& r, entt::entity e) {
    if (const auto* n = r.try_get<scene::Name>(e); n != nullptr && !n->value.empty()) {
        return n->value;
    }
    return "entity " + std::to_string(static_cast<std::uint32_t>(e));
}

} // namespace

void draw_status_bar(EditorShell& shell) {
    const ui::Theme& t = ui::tokens();
    const EditorContext* ctx = shell.ctx;
    const float height = ui::scaled(ui::Sizes::status_height + 4.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::to_imvec4(t.bg0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::scaled(ui::Spacing::xs), ui::scaled(2.0F)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(ui::scaled(ui::Spacing::xs), ui::scaled(2.0F)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##status_bar", ImGui::GetMainViewport(), ImGuiDir_Down, height,
                                    flags)) {
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y),
                                            ui::to_u32(t.border));
        ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5F);

        // Simulation state.
        switch (shell.sim_state()) {
        case SimState::editing: pill(icon::wrench, "Editing", t.text_secondary); break;
        case SimState::playing: pill(icon::play, "Playing", t.success); break;
        case SimState::paused: pill(icon::pause, "Paused", t.warning); break;
        }
        ui::item_tooltip("Simulation state (F5 play, F6 pause, F10 step, Shift+F5 stop)");

        // Selection + entity count.
        if (ctx != nullptr && ctx->scene != nullptr) {
            const entt::registry& r = ctx->scene->registry();
            separator_dot();
            ImGui::AlignTextToFramePadding();
            if (shell.has_selection()) {
                ui::text_colored(t.text, ui::label(icon::mouse_pointer_2, entity_name(r, shell.selected)));
            } else {
                ui::text_colored(t.text_disabled, ui::label(icon::mouse_pointer_2, "No selection"));
            }
            separator_dot();
            char count[48];
            std::snprintf(count, sizeof(count), "%d entities", ctx->entity_count);
            ui::text_colored(t.text_secondary, ui::label(icon::boxes, count));
        }

        // Right-aligned: remote state, frame timing.
        char timing[64];
        const float fps = ImGui::GetIO().Framerate;
        std::snprintf(timing, sizeof(timing), "%5.1f fps  %6.2f ms", static_cast<double>(fps),
                      fps > 0.0F ? 1000.0 / static_cast<double>(fps) : 0.0);
        char remote[64];
        ui::Rgba remote_col = t.text_disabled;
        const char* remote_icon = icon::plug;
        if (ctx != nullptr && ctx->remote.enabled) {
            std::snprintf(remote, sizeof(remote), "MCP :%u  %zu client%s",
                          static_cast<unsigned>(ctx->remote.port), ctx->remote.clients,
                          ctx->remote.clients == 1 ? "" : "s");
            remote_col = ctx->remote.clients > 0 ? t.success : t.text_secondary;
            remote_icon = icon::radio_tower;
        } else {
            std::snprintf(remote, sizeof(remote), "Remote off");
        }
        const std::string remote_text = ui::label(remote_icon, remote);
        float timing_w = 0.0F;
        {
            ui::FontScope mono(ui::FontRole::mono);
            timing_w = ImGui::CalcTextSize(timing).x;
        }
        const float remote_w = ImGui::CalcTextSize(remote_text.c_str()).x;
        const float right_w = timing_w + remote_w + ui::scaled(ui::Spacing::lg + ui::Spacing::xs);
        const float right_x = ImGui::GetWindowWidth() - right_w;

        // Newest log line fills the middle; click opens the Console.
        if (shell.console != nullptr) {
            if (const core::LogLine* line = shell.console->model.last()) {
                separator_dot();
                const float x0 = ImGui::GetCursorPosX();
                const float avail = right_x - x0 - ui::scaled(ui::Spacing::md);
                if (avail > ui::scaled(80.0F)) {
                    const ui::Rgba col = line->level == core::LogLevel::error  ? t.error
                                         : line->level == core::LogLevel::warn ? t.warning
                                                                                : t.text_secondary;
                    const std::string text = ui::label(
                        line->level >= core::LogLevel::warn ? icon::triangle_alert : icon::terminal,
                        (line->category.empty() ? "" : "[" + line->category + "] ") + line->message);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##last_log", ImVec2(avail, ImGui::GetFrameHeight()))) {
                        shell.show_panel(Panel::console);
                    }
                    const bool hovered = ImGui::IsItemHovered();
                    const float h = ImGui::GetFrameHeight();
                    const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5F;
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ui::to_imvec4(hovered ? t.text : col));
                    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(p.x, ty),
                                              ImVec2(p.x + avail, p.y + h), p.x + avail,
                                              text.c_str(), nullptr, nullptr);
                    ImGui::PopStyleColor();
                    if (hovered) ImGui::SetTooltip("Open the Console");
                }
            }
        }

        ImGui::SameLine(right_x);
        ImGui::AlignTextToFramePadding();
        ui::text_colored(remote_col, remote_text);
        ui::item_tooltip("Remote control (QUAT_REMOTE): the quat-mcp server connects here");
        ImGui::SameLine(0.0F, ui::scaled(ui::Spacing::lg));
        {
            ui::FontScope mono(ui::FontRole::mono);
            ui::text_colored(t.text_secondary, timing);
        }
        if (ctx != nullptr) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "CPU frame %.2f ms, %d draws", static_cast<double>(ctx->frame_ms),
                          ctx->draw_count);
            ui::item_tooltip(detail);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

} // namespace engine::editor::shell
