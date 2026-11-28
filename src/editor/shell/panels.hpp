#pragma once

// The editor's dockable panels (ROADMAP E8.2): one table of titles and
// icons, and the ImGui window names built from it. A window name is
// "<icon>  <Title>###<Title>": the tab shows the icon, while the ID after
// "###" stays the bare title, so docking, focusing (QUAT_EDITOR_FOCUS) and
// saved layouts do not depend on the icon font.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor::shell {

enum class Panel : std::uint8_t {
    viewport,
    hierarchy,
    inspector,
    assets,
    console,
    animation,
    behaviour,
    physics,
    renderer,
    stats,
};
inline constexpr std::size_t k_panel_count = 10;

struct PanelInfo {
    Panel       id;
    const char* title;     // also the window ID ("###<title>")
    const char* icon;      // icon:: constant
    bool        closable;  // listed as a View toggle (else View focuses it)
};

[[nodiscard]] const std::array<PanelInfo, k_panel_count>& panel_infos();
[[nodiscard]] const PanelInfo& panel_info(Panel p);
// "<icon>  <Title>###<Title>" (without the icon when the font is missing).
[[nodiscard]] std::string panel_window_name(Panel p);
// "###<Title>": the window's ID string (DockBuilder, SetWindowFocus).
[[nodiscard]] std::string panel_window_id(Panel p);
// Case-insensitive lookup by title ("assets" -> Panel::assets).
[[nodiscard]] std::optional<Panel> find_panel(std::string_view title);

} // namespace engine::editor::shell
