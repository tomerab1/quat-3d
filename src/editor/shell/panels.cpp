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
}};

struct Names {
    bool                                   icons = false;
    bool                                   built = false;
    std::array<std::string, k_panel_count> window;
    std::array<std::string, k_panel_count> id;
};
Names g_names;
std::array<bool, k_panel_count>* g_open_flags = nullptr;

void build_names() {
    const bool icons = ui::fonts().icons;
    if (g_names.built && g_names.icons == icons) return;
    for (std::size_t i = 0; i < k_panel_count; ++i) {
        const PanelInfo& p = k_panels[i];
        const char* glyph = ui::ic(p.icon);
        g_names.window[i] = std::string(glyph) + (glyph[0] != '\0' ? "  " : "") + p.title +
                            "###" + p.title;
        g_names.id[i] = std::string("###") + p.title;
    }
    g_names.icons = icons;
    g_names.built = true;
}

} // namespace

const std::array<PanelInfo, k_panel_count>& panel_infos() { return k_panels; }

const PanelInfo& panel_info(Panel p) { return k_panels[static_cast<std::size_t>(p)]; }

const char* panel_window_name(Panel p) {
    build_names();
    return g_names.window[static_cast<std::size_t>(p)].c_str();
}

const char* panel_window_id(Panel p) {
    build_names();
    return g_names.id[static_cast<std::size_t>(p)].c_str();
}

bool* panel_open_flag(Panel p) {
    if (g_open_flags == nullptr || !panel_info(p).closable) return nullptr;
    return &(*g_open_flags)[static_cast<std::size_t>(p)];
}

void set_panel_open_flags(std::array<bool, k_panel_count>* flags) { g_open_flags = flags; }

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
