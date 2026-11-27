// Stats panel: frame numbers and the shader hot-reload state (R2). One row
// per Slang source; failed shaders open by default with the compiler
// diagnostics (the previous pipeline stays live).

#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"
#include "../script_inspector.hpp"

namespace engine::editor::shell {

namespace {

[[nodiscard]] ui::Rgba state_color(rhi::ShaderStatus::State s) {
    const ui::Theme& t = ui::tokens();
    switch (s) {
    case rhi::ShaderStatus::State::error: return t.error;
    case rhi::ShaderStatus::State::ok: return t.success;
    case rhi::ShaderStatus::State::compiling: return t.warning;
    case rhi::ShaderStatus::State::needs_restart: return t.info;
    case rhi::ShaderStatus::State::cooked: return t.text_disabled;
    }
    return t.text;
}

void shader_status(const std::vector<rhi::ShaderStatus>& shaders) {
    std::size_t errors = 0;
    std::size_t compiling = 0;
    std::size_t restart = 0;
    for (const rhi::ShaderStatus& s : shaders) {
        if (s.state == rhi::ShaderStatus::State::error) ++errors;
        if (s.state == rhi::ShaderStatus::State::compiling) ++compiling;
        if (s.state == rhi::ShaderStatus::State::needs_restart) ++restart;
    }
    char header[128];
    std::snprintf(header, sizeof(header), "Shaders  %zu  (%zu error%s%s%s)###shaders",
                  shaders.size(), errors, errors == 1 ? "" : "s", compiling > 0 ? ", compiling" : "",
                  restart > 0 ? ", restart to apply" : "");
    if (errors > 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (!ui::section_header(header, false)) return;

    ui::FontScope mono(ui::FontRole::mono);
    for (const rhi::ShaderStatus& s : shaders) {
        ImGui::PushID(s.name.c_str());
        const std::string_view state = rhi::to_string(s.state);
        char tag[24];
        std::snprintf(tag, sizeof(tag), "%-9.*s", static_cast<int>(state.size()), state.data());
        ui::text_colored(state_color(s.state), tag);
        ImGui::SameLine();
        if (s.reloads > 0) {
            ImGui::Text("%s  (x%u, %u pipeline%s)", s.name.c_str(), s.reloads, s.pipelines,
                        s.pipelines == 1 ? "" : "s");
        } else {
            ImGui::TextUnformatted(s.name.c_str());
        }
        if (!s.message.empty()) {
            const bool failed = s.state == rhi::ShaderStatus::State::error;
            const bool restart_note = s.state == rhi::ShaderStatus::State::needs_restart;
            if (failed || restart_note) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode(failed ? "diagnostics" : restart_note ? "not applied" : "warnings")) {
                ImGui::PushTextWrapPos(0.0F);
                ui::text_colored(failed ? ui::tokens().error : ui::tokens().text, s.message);
                ImGui::PopTextWrapPos();
                if (ui::button(ui::label(icon::copy, "Copy").c_str())) {
                    ImGui::SetClipboardText(s.message.c_str());
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
}

} // namespace

void draw_stats_panel(EditorShell& shell) {
    bool& open = shell.panel_open[static_cast<std::size_t>(Panel::stats)];
    if (!open) return;
    if (!ImGui::Begin(panel_window_name(Panel::stats).c_str(), &open)) {
        ImGui::End();
        return;
    }
    const EditorContext& ctx = *shell.ctx;
    if (ui::begin_property_grid("frame")) {
        const float fps = ImGui::GetIO().Framerate;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f fps (%.2f ms)", static_cast<double>(fps),
                      fps > 0.0F ? 1000.0 / static_cast<double>(fps) : 0.0);
        ui::prop_value("Frame rate", buf, "ImGui's moving average", true);
        std::snprintf(buf, sizeof(buf), "%.2f ms", static_cast<double>(ctx.frame_ms));
        ui::prop_value("Frame time", buf, "This frame's real time", true);
        std::snprintf(buf, sizeof(buf), "%d", ctx.draw_count);
        ui::prop_value("Draws", buf, "Opaque + transparent draw items", true);
        std::snprintf(buf, sizeof(buf), "%d", ctx.entity_count);
        ui::prop_value("Entities", buf, nullptr, true);
        std::snprintf(buf, sizeof(buf), "%u x %u", shell.viewport_width, shell.viewport_height);
        ui::prop_value("Viewport", buf, "Scene render size", true);
        ui::end_property_grid();
    }
    if (ctx.shader_statuses != nullptr) {
        shader_status(*ctx.shader_statuses);
    } else {
        ui::text_colored(ui::tokens().text_disabled, "Shader hot reload is off");
    }
    draw_script_status(ctx.scripts); // G7: per-file script state
    ImGui::End();
}

} // namespace engine::editor::shell
