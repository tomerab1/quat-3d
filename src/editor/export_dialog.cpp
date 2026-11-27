#include "export_dialog.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <SDL3/SDL_misc.h>
#include <imgui.h>

#include "engine/core/utf8_path.hpp"
#include "engine/editor/editor.hpp"
#include "engine/project/build_job.hpp"
#include "engine/project/game_project.hpp"

namespace engine::editor {

namespace {

constexpr float k_toast_seconds = 8.0F;

template <std::size_t N>
void copy_into(std::array<char, N>& dst, const std::string& src) {
    const std::size_t n = std::min(src.size(), N - 1);
    std::copy_n(src.data(), n, dst.data());
    dst[n] = '\0';
}

void show_in_folder(const std::string& folder) {
    if (!SDL_OpenURL(file_url(folder).c_str())) {
        std::fprintf(stderr, "[editor] cannot open '%s': %s\n", folder.c_str(), SDL_GetError());
    }
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

void ExportDialog::open(const EditorContext& ctx) {
    visible_ = true;
    start_error_.clear();
    if (project_[0] == '\0') {
        copy_into(project_, ctx.game != nullptr ? ctx.game_root : std::string("games/coin-collector"));
    }
    if (output_[0] == '\0') copy_into(output_, "dist");
}

void ExportDialog::draw(const EditorContext& ctx) {
    if (ctx.build_job == nullptr) return;
    // The log is only copied while the window shows it.
    const project::BuildStatus status = ctx.build_job->status(/*with_log=*/visible_);
    if (visible_) draw_window(ctx, status);
    draw_toast(status);
}

void ExportDialog::draw_window(const EditorContext& ctx, const project::BuildStatus& status) {
    ImGui::SetNextWindowSize(ImVec2(560.0F, 520.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Build & Export", &visible_)) {
        ImGui::End();
        return;
    }
    const bool running = status.state == project::BuildState::running;

    // ---- Project -------------------------------------------------------
    if (ctx.game != nullptr) {
        ImGui::Text("%s %s", ctx.game->name.c_str(), ctx.game->version.c_str());
        ImGui::TextDisabled("%s  -  %s", ctx.game->id.c_str(), ctx.game_root.c_str());
    } else {
        ImGui::BeginDisabled(running);
        ImGui::InputText("Game project", project_.data(), project_.size());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Folder holding game.json (relative to the engine root, or absolute)");
    }
    ImGui::Separator();

    // ---- Settings ------------------------------------------------------
    ImGui::BeginDisabled(running);
    const std::string host(project::host_target());
    if (ImGui::BeginCombo("Target", host.c_str())) {
        ImGui::Selectable(host.c_str(), true);
        for (const char* other : {"linux-x64", "windows-x64", "macos-arm64"}) {
            if (host == other) continue;
            ImGui::BeginDisabled();
            ImGui::Selectable(other, false);
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("A package is built on its own OS; CI packages all three");
    ImGui::InputText("Output folder", output_.data(), output_.size());
    ImGui::SetItemTooltip("Absolute, or relative to the game project");
    ImGui::RadioButton("Shipping", &config_, 0);
    ImGui::SetItemTooltip("Remote control stays off in the packaged game");
    ImGui::SameLine();
    ImGui::RadioButton("Development", &config_, 1);
    ImGui::SetItemTooltip("The packaged game accepts QUAT_REMOTE (MCP can drive it)");
    ImGui::Checkbox("Smoke-test the package", &smoke_test_);
    ImGui::EndDisabled();

    // ---- Run -----------------------------------------------------------
    if (running) {
        if (ImGui::Button("Cancel")) ctx.build_job->cancel();
    } else if (ImGui::Button("Build")) {
        ExportRequest req;
        req.project = ctx.game != nullptr ? std::string() : std::string(project_.data());
        req.output = output_.data();
        req.config = config_ == 0 ? project::PackageConfig::shipping
                                  : project::PackageConfig::development;
        req.smoke_test = smoke_test_;
        auto started = ctx.start_export(req);
        start_error_ = started ? std::string() : started.error().message;
    }
    if (!start_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.35F, 1.0F), "%s", start_error_.c_str());
    }

    if (status.state != project::BuildState::idle) {
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
        case project::BuildState::succeeded:
            ImGui::TextColored(ImVec4(0.45F, 0.85F, 0.45F, 1.0F), "Packaged in %.1f s",
                               status.elapsed_s);
            if (!status.folder.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Show in folder")) show_in_folder(status.folder);
            }
            break;
        case project::BuildState::failed:
        case project::BuildState::cancelled:
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.35F, 1.0F), "%s", status.error.c_str());
            ImGui::PopTextWrapPos();
            break;
        default:
            break;
        }
    }

    // ---- Log -----------------------------------------------------------
    ImGui::TextDisabled("Log");
    if (ImGui::BeginChild("log", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(status.log.data(), status.log.data() + status.log.size());
        if (running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
            ImGui::SetScrollHereY(1.0F); // follow the tail while building
        }
        if (!status.log.empty() && ImGui::BeginPopupContextWindow()) {
            if (ImGui::MenuItem("Copy log")) ImGui::SetClipboardText(status.log.c_str());
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void ExportDialog::draw_toast(const project::BuildStatus& status) {
    // TODO(E8): use the design system's toasts once they land.
    if (status.state == project::BuildState::succeeded && status.job != toasted_job_) {
        toasted_job_ = status.job;
        toast_folder_ = status.folder;
        toast_text_ = "Packaged " + core::path_to_utf8(core::path_from_utf8(status.package).filename());
        toast_seconds_ = k_toast_seconds;
    }
    if (toast_seconds_ <= 0.0F) return;
    toast_seconds_ -= ImGui::GetIO().DeltaTime;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16.0F,
                                   vp->WorkPos.y + vp->WorkSize.y - 16.0F),
                            ImGuiCond_Always, ImVec2(1.0F, 1.0F));
    ImGui::SetNextWindowBgAlpha(0.92F);
    constexpr ImGuiWindowFlags k_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##export_toast", nullptr, k_flags)) {
        ImGui::TextUnformatted(toast_text_.c_str());
        if (!toast_folder_.empty() && ImGui::SmallButton("Show in folder")) {
            show_in_folder(toast_folder_);
            toast_seconds_ = 0.0F;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) toast_seconds_ = 0.0F;
        if (ImGui::IsWindowHovered()) toast_seconds_ = std::max(toast_seconds_, 1.0F);
    }
    ImGui::End();
}

} // namespace engine::editor
