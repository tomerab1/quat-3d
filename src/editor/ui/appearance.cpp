#include "appearance.hpp"

#include <cstdio>

#include <SDL3/SDL_video.h>
#include <imgui.h>

#include "fonts.hpp"

namespace engine::editor::ui {

DpiScale window_dpi(SDL_Window* window) {
    if (window == nullptr) return {};
    return dpi_scale(SDL_GetWindowDisplayScale(window), SDL_GetWindowPixelDensity(window));
}

void Appearance::init(SDL_Window* window, const std::filesystem::path& project_root,
                      ThemeId theme) {
    window_ = window;
    font_dir_ = project_root / "assets" / "fonts";
    theme_ = theme;
    pending_theme_ = theme;
    dpi_ = window_dpi(window);
    rebuild_fonts();
    apply_theme(ui::theme(theme_), dpi_.ui);
}

bool Appearance::update() {
    if (pending_theme_ != theme_) {
        theme_ = pending_theme_;
        apply_theme(ui::theme(theme_), dpi_.ui);
    }
    const DpiScale now = window_dpi(window_);
    if (!dpi_changed(dpi_, now)) return false;
    std::fprintf(stderr, "[editor] display scale %.2f -> %.2f (density %.2f): rebuilding fonts\n",
                 static_cast<double>(dpi_.ui), static_cast<double>(now.ui),
                 static_cast<double>(now.raster));
    dpi_ = now;
    rebuild_fonts();
    apply_theme(ui::theme(theme_), dpi_.ui);
    return true;
}

void Appearance::set_theme(ThemeId id) { pending_theme_ = id; }

void Appearance::rebuild_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    (void)load_fonts(*io.Fonts, font_dir_, dpi_);
    // The base size every PushFont(nullptr, 0) falls back to is the body
    // font's baked size.
    ImGui::GetStyle().FontSizeBase = 0.0F;
}

} // namespace engine::editor::ui
