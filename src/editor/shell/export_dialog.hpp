#pragma once

// File > Build & Export... (ROADMAP E7, the editor side of P4 game
// packaging), in the E8 shell. Shows the project, the target (the host OS: a
// package is built on its own OS), the output folder and the config; starts
// quat-pack in the background through EditorContext::start_export and shows
// its progress and log. A finished build raises a toast ("Show in folder";
// a failed one: "Show log").

#include <cstdint>
#include <string>

#include "engine/project/package_manifest.hpp"

namespace engine::project {
struct BuildStatus;
}
namespace engine::editor {
struct EditorContext;
}

namespace engine::editor::shell {

struct ExportDialogState {
    bool                   open_request = false; // file.export: show (and seed) next frame
    bool                   visible = false;
    std::string            project; // UTF-8; used when no game project is open
    std::string            output = "dist";
    project::PackageConfig config = project::PackageConfig::shipping;
    bool                   smoke_test = false;
    std::string            start_error;
    std::uint64_t          notified_job = 0; // the last finished job a toast was raised for
};

// The window (when visible) and the completion toasts (always). Draws
// nothing without a build job and start_export (EditorContext).
void draw_export_dialog(ExportDialogState& state, const EditorContext& ctx);

// Raises the toast for a build that finished since the last call (once per
// job): success offers "Show in folder", failure "Show log" (reopens the
// window). Running, cancelled and idle builds raise nothing.
void notify_build_finished(ExportDialogState& state, const project::BuildStatus& status);

// file:// URL for a local path (UTF-8), percent-encoded — "Show in folder".
[[nodiscard]] std::string file_url(const std::string& utf8_path);

} // namespace engine::editor::shell
