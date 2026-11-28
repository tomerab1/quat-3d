#include "export_dialog.hpp"

#include <cstdio>
#include <span>
#include <string>

#include <SDL3/SDL_misc.h>
#include <imgui.h>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/theme.hpp"
#include "../ui/toasts.hpp"
#include "../ui/widgets.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/editor/editor.hpp"
#include "engine/project/build_job.hpp"
#include "engine/project/game_project.hpp"

namespace engine::editor::shell {

namespace {

constexpr float k_toast_seconds = 8.0F;

void show_in_folder(const std::string& folder) {
    if (!SDL_OpenURL(file_url(folder).c_str())) {
        std::fprintf(stderr, "[editor] cannot open '%s': %s\n", folder.c_str(), SDL_GetError());
    }
}

// Fields seeded when the window opens: the open project, else the sample.
void seed(ExportDialogState& s, const EditorContext& ctx) {
    s.visible = true;
    s.start_error.clear();
    if (s.project.empty()) s.project = ctx.game != nullptr ? ctx.game_root : "games/coin-collector";
    if (s.output.empty()) s.output = "dist";
}

} // namespace

void notify_build_finished(ExportDialogState& s, const project::BuildStatus& status) {
    const bool done = status.state == project::BuildState::succeeded ||
                      status.state == project::BuildState::failed;
    if (!done || status.job == s.notified_job) return;
    s.notified_job = status.job;
    if (status.state == project::BuildState::succeeded) {
        const std::string name =
            core::path_to_utf8(core::path_from_utf8(status.package).filename());
        ui::ToastAction action;
        if (!status.folder.empty()) {
            action = {.label = "Show in folder",
                      .run = [folder = status.folder] { show_in_folder(folder); }};
        }
        ui::notify(ui::ToastKind::success, "Packaged " + name, k_toast_seconds, std::move(action));
    } else {
        ui::notify(ui::ToastKind::error, "Build & Export failed: " + status.error, k_toast_seconds,
                   {.label = "Show log", .run = [&s] { s.visible = true; }});
    }
}

namespace {

void project_rows(ExportDialogState& s, const EditorContext& ctx, bool running) {
    if (ctx.game != nullptr) {
        ui::prop_value("Game", ctx.game->name + " " + ctx.game->version);
        ui::prop_value("Id", ctx.game->id, nullptr, /*mono=*/true);
        ui::prop_value("Folder", ctx.game_root, nullptr, /*mono=*/true);
        return;
    }
    (void)ui::prop_text("Game project", s.project,
                        {.tooltip = "Folder holding game.json (relative to the engine root, "
                                    "or absolute)",
                         .read_only = running});
}

void settings_rows(ExportDialogState& s, bool running) {
    ui::prop_value("Target", project::host_target(),
                   "A package is built on its own OS; CI packages all three");
    (void)ui::prop_text("Output folder", s.output,
                        {.tooltip = "Absolute, or relative to the game project",
                         .read_only = running});
    static constexpr ui::EnumItem configs[] = {
        {static_cast<int>(project::PackageConfig::shipping), "Shipping",
         "Remote control is compiled out of the packaged game"},
        {static_cast<int>(project::PackageConfig::development), "Development",
         "The packaged game accepts QUAT_REMOTE (MCP can drive it)"},
    };
    (void)ui::prop_enum("Config", s.config, std::span<const ui::EnumItem>(configs),
                        {.tooltip = "What the package contains", .read_only = running});
    (void)ui::prop_bool("Smoke test", s.smoke_test,
                        {.tooltip = "Run the packaged game headless for a few frames",
                         .read_only = running});
}

void run_row(ExportDialogState& s, const EditorContext& ctx, const project::BuildStatus& status) {
    const bool running = status.state == project::BuildState::running;
    if (running) {
        if (ui::button(ui::label(icon::square, "Cancel").c_str(), ui::ButtonKind::danger)) {
            ctx.build_job->cancel();
        }
    } else if (ui::button(ui::label(icon::package, "Build").c_str(), ui::ButtonKind::primary)) {
        ExportRequest req;
        req.project = ctx.game != nullptr ? std::string() : s.project;
        req.output = s.output;
        req.config = s.config;
        req.smoke_test = s.smoke_test;
        auto started = ctx.start_export(req);
        s.start_error = started ? std::string() : started.error().message;
    }
    if (!s.start_error.empty()) ui::text_colored(ui::tokens().error, s.start_error);
    if (status.state == project::BuildState::idle) return;

    char overlay[160];
    if (status.step > 0) {
        std::snprintf(overlay, sizeof(overlay), "%u/%u %s", status.step, status.steps,
                      status.step_name.c_str());
    } else {
        std::snprintf(overlay, sizeof(overlay), "%s",
                      std::string(project::to_string(status.state)).c_str());
    }
    ImGui::ProgressBar(status.progress(), ImVec2(-1.0F, 0.0F), overlay);
    switch (status.state) {
    case project::BuildState::succeeded: {
        char done[64];
        std::snprintf(done, sizeof(done), "Packaged in %.1f s", status.elapsed_s);
        ui::text_colored(ui::tokens().success, done);
        if (!status.folder.empty()) {
            ImGui::SameLine();
            if (ui::button(ui::label(icon::folder_open, "Show in folder").c_str(),
                           ui::ButtonKind::ghost)) {
                show_in_folder(status.folder);
            }
        }
        break;
    }
    case project::BuildState::failed:
    case project::BuildState::cancelled:
        ImGui::PushTextWrapPos(0.0F);
        ui::text_colored(ui::tokens().error, status.error);
        ImGui::PopTextWrapPos();
        break;
    default: break;
    }
}

void log_view(const project::BuildStatus& status) {
    ImGui::SeparatorText("Log");
    if (ImGui::BeginChild("##log", ImVec2(0.0F, ui::scaled(220.0F)), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (status.log.empty()) {
            ui::text_secondary("quat-pack's output appears here.");
        } else {
            ui::FontScope mono(ui::FontRole::mono);
            ImGui::TextUnformatted(status.log.data(), status.log.data() + status.log.size());
        }
        if (status.state == project::BuildState::running &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
            ImGui::SetScrollHereY(1.0F); // follow the tail while building
        }
        if (!status.log.empty() && ImGui::BeginPopupContextWindow()) {
            if (ImGui::MenuItem(ui::label(icon::copy, "Copy log").c_str())) {
                ImGui::SetClipboardText(status.log.c_str());
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

void export_window(ExportDialogState& s, const EditorContext& ctx,
                   const project::BuildStatus& status) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(ui::scaled(560.0F), 0.0F), ImGuiCond_Appearing);
    if (ImGui::Begin(ui::label(icon::package, "Build & Export###build_export").c_str(), &s.visible,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        const bool running = status.state == project::BuildState::running;
        if (ui::begin_property_grid("export")) {
            project_rows(s, ctx, running);
            ui::prop_separator("Package");
            settings_rows(s, running);
            ui::end_property_grid();
        }
        ImGui::Spacing();
        run_row(s, ctx, status);
        log_view(status);
    }
    ImGui::End();
}

} // namespace

std::string file_url(const std::string& utf8_path) {
    const std::string generic = core::path_to_generic_utf8(core::path_from_utf8(utf8_path));
    std::string url = "file://";
    if (!generic.starts_with("/")) url += "/"; // Windows: file:///C:/...
    constexpr char k_hex[] = "0123456789ABCDEF";
    for (const char ch : generic) {
        const auto c = static_cast<unsigned char>(ch);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
                           c == '.' || c == '~' || c == ':';
        if (plain) {
            url.push_back(ch);
        } else {
            url.push_back('%');
            url.push_back(k_hex[c >> 4]);
            url.push_back(k_hex[c & 0xF]);
        }
    }
    return url;
}

void draw_export_dialog(ExportDialogState& s, const EditorContext& ctx) {
    if (ctx.build_job == nullptr || !ctx.start_export) return;
    if (s.open_request) {
        s.open_request = false;
        seed(s, ctx);
    }
    // The log is only copied while the window shows it.
    const project::BuildStatus status = ctx.build_job->status(/*with_log=*/s.visible);
    if (s.visible) export_window(s, ctx, status);
    notify_build_finished(s, status);
}

} // namespace engine::editor::shell
