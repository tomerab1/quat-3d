#pragma once

// File > Build & Export... (ROADMAP E7, the editor side of P4 game
// packaging). Picks the target (the host OS: a package is built on its own
// OS), the output folder and the config, starts quat-pack in the background
// through EditorContext::start_export, and shows its progress and log. A
// finished build raises a toast with "Show in folder".
//
// Kept deliberately plain until the E8 design system lands: then the toast
// moves to E8's toasts and the menu entry to its command registry.

#include <array>
#include <cstdint>
#include <string>

namespace engine::project {
struct BuildStatus;
}

namespace engine::editor {

struct EditorContext;

class ExportDialog {
public:
    // Shows the window, seeding the fields from the open game project.
    void open(const EditorContext& ctx);
    // The window (when open) and the completion toast (always).
    void draw(const EditorContext& ctx);

private:
    void draw_window(const EditorContext& ctx, const project::BuildStatus& status);
    void draw_toast(const project::BuildStatus& status);

    bool                     visible_ = false;
    std::array<char, 1024>   project_{};
    std::array<char, 1024>   output_{};
    int                      config_ = 0; // 0 shipping, 1 development
    bool                     smoke_test_ = false;
    std::string              start_error_;
    std::uint64_t            toasted_job_ = 0;
    // The toast for a finished job: its package folder and time left.
    std::string toast_folder_;
    std::string toast_text_;
    float       toast_seconds_ = 0.0F;
};

// file:// URL for a local path (UTF-8), percent-encoded — "Show in folder".
[[nodiscard]] std::string file_url(const std::string& utf8_path);

} // namespace engine::editor
