#pragma once

// Appearance — owns the editor's look at runtime (ROADMAP E8.1): the active
// theme, the DPI scale and the fonts built for it. Once per frame, before
// ImGui::NewFrame, update() compares the window's SDL3 display scale with
// the one the fonts were built for and rebuilds fonts + style on a change
// (window moved to another monitor, OS scale changed); the renderer then
// re-uploads the atlas (EditorLayer::take_font_atlas_changed).

#include <filesystem>

#include "metrics.hpp"
#include "theme.hpp"

struct SDL_Window;

namespace engine::editor::ui {

class Appearance {
public:
    // Builds fonts from `<project_root>/assets/fonts` for the window's
    // current scale and applies `theme`. Needs a current ImGui context.
    void init(SDL_Window* window, const std::filesystem::path& project_root, ThemeId theme);

    // Rebuilds fonts and style when the window's scale changed; returns true
    // when the font atlas was rebuilt (the renderer must re-upload it).
    [[nodiscard]] bool update();

    // Takes effect at the next update() (style changes mid-frame would be
    // undone by the style stack).
    void set_theme(ThemeId id);
    [[nodiscard]] ThemeId theme_id() const { return pending_theme_; }
    [[nodiscard]] DpiScale dpi() const { return dpi_; }

private:
    void rebuild_fonts();

    SDL_Window*           window_ = nullptr;
    std::filesystem::path font_dir_;
    DpiScale              dpi_{};
    ThemeId               theme_ = ThemeId::dark;
    ThemeId               pending_theme_ = ThemeId::dark;
};

// The SDL3 scale of `window` (dpi_scale of its display scale and pixel
// density); {1, 1} for a null window.
[[nodiscard]] DpiScale window_dpi(SDL_Window* window);

} // namespace engine::editor::ui
