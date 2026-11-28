#include "panels.hpp"

#include <cctype>
#include <string>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"

namespace engine::editor::shell {

namespace {

constexpr std::array<PanelInfo, k_panel_count> k_panels = {{
    {Panel::viewport, "Viewport", icon::video, false},
    {Panel::hierarchy, "Hierarchy", icon::list_tree, true},
    {Panel::inspector, "Inspector", icon::sliders_horizontal, true},
    {Panel::assets, "Assets", icon::folder, true},
    {Panel::console, "Console", icon::terminal, true},
    {Panel::animation, "Animation", icon::clapperboard, false},
    {Panel::behaviour, "Behaviour", icon::workflow, false},
    {Panel::physics, "Physics", icon::atom, false},
    {Panel::renderer, "Renderer", icon::sun_medium, false},
    {Panel::stats, "Stats", icon::activity, true},
    {Panel::profiler, "Profiler", icon::gauge, true},
}};

} // namespace

const std::array<PanelInfo, k_panel_count>& panel_infos() { return k_panels; }

const PanelInfo& panel_info(Panel p) { return k_panels[static_cast<std::size_t>(p)]; }

std::string panel_window_name(Panel p) {
    const PanelInfo& info = panel_info(p);
    const char* glyph = ui::ic(info.icon);
    return std::string(glyph) + (glyph[0] != '\0' ? "  " : "") + info.title + "###" + info.title;
}

std::string panel_window_id(Panel p) { return std::string("###") + panel_info(p).title; }

std::optional<Panel> find_panel(std::string_view title) {
    for (const PanelInfo& p : k_panels) {
        const std::string_view t(p.title);
        if (t.size() != title.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < t.size() && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(t[i])) ==
                   std::tolower(static_cast<unsigned char>(title[i]));
        }
        if (same) return p.id;
    }
    return std::nullopt;
}

} // namespace engine::editor::shell
