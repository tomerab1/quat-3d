#include "asset_browser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL_misc.h>
#include <imgui.h>

#include "shell/panels.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/metrics.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

namespace fs = std::filesystem;
using Entry = AssetBrowserState::Entry;

constexpr std::size_t k_max_search_results = 2000;
constexpr auto        k_refresh_interval = std::chrono::seconds(2);
constexpr const char* k_roots[] = {"assets", "sample-assets", "scenes"};

[[nodiscard]] std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return {s.begin(), s.end()};
}

[[nodiscard]] std::string lower_ext(const fs::path& p) {
    std::string e = utf8(p.extension());
    std::transform(e.begin(), e.end(), e.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return e;
}

void list_dir(const fs::path& dir, std::vector<Entry>& out) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = utf8(entry.path().filename());
        if (name.empty() || name.front() == '.') continue;
        Entry e;
        e.path = entry.path();
        e.name = name;
        e.is_dir = entry.is_directory(ec);
        e.size = e.is_dir ? 0 : entry.file_size(ec);
        out.push_back(std::move(e));
    }
}

void search_dir(const fs::path& root, const fs::path& dir, const std::string& filter,
                std::vector<Entry>& out) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (out.size() >= k_max_search_results) return;
        const std::string name = utf8(it->path().filename());
        if (!name.empty() && name.front() == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!ui::matches_filter(name, filter)) continue;
        Entry e;
        e.path = it->path();
        e.name = utf8(it->path().lexically_relative(root));
        e.is_dir = it->is_directory(ec);
        e.size = e.is_dir ? 0 : it->file_size(ec);
        out.push_back(std::move(e));
    }
}

// Refreshes the listing when the folder or the search changed, or it is stale.
void refresh(AssetBrowserState& s, const fs::path& root) {
    const auto now = std::chrono::steady_clock::now();
    if (!s.dirty && s.cached_dir == s.dir && s.cached_filter == s.filter &&
        now - s.cached_at < k_refresh_interval) {
        return;
    }
    s.entries.clear();
    if (s.dir.empty()) {
        // Top level: the project's asset roots that exist.
        for (const char* sub : k_roots) {
            std::error_code ec;
            if (fs::is_directory(root / sub, ec)) s.entries.push_back({root / sub, sub, true, 0});
        }
        if (!s.filter.empty()) {
            std::vector<Entry> found;
            for (const char* sub : k_roots) search_dir(root, root / sub, s.filter, found);
            s.entries = std::move(found);
        }
    } else if (s.filter.empty()) {
        list_dir(root / s.dir, s.entries);
    } else {
        search_dir(root, root / s.dir, s.filter, s.entries);
    }
    std::sort(s.entries.begin(), s.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
    s.cached_dir = s.dir;
    s.cached_filter = s.filter;
    s.cached_at = now;
    s.dirty = false;
}

void navigate(AssetBrowserState& s, const fs::path& rel) {
    s.dir = rel;
    s.filter.clear();
    s.dirty = true;
}

[[nodiscard]] std::string human_size(std::uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%ju B", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

[[nodiscard]] ui::Rgba kind_tint(AssetKind k) {
    const ui::Theme& t = ui::tokens();
    switch (k) {
    case AssetKind::folder: return t.warning;
    case AssetKind::model: return t.accent_text;
    case AssetKind::texture: return t.success;
    case AssetKind::shader: return t.info;
    case AssetKind::scene: return t.accent_text;
    case AssetKind::audio: return t.success;
    default: return t.text_secondary;
    }
}

// Shared item behaviour: drag source, double-click, context menu, tooltip.
// Returns true when the item asks to open a folder.
bool item_behaviour(const Entry& e, const AssetTypeInfo& type, std::string& instantiate) {
    bool open_folder = false;
    if (!e.is_dir && ImGui::BeginDragDropSource()) {
        const std::string path = utf8(e.path);
        ImGui::SetDragDropPayload(asset_drag_payload, path.c_str(), path.size() + 1);
        ImGui::TextUnformatted(ui::label(type.icon, e.name).c_str());
        if (type.kind == AssetKind::model) {
            ui::text_secondary("Drop on the viewport to place it");
        }
        ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (e.is_dir) {
            open_folder = true;
        } else if (type.kind == AssetKind::model) {
            instantiate = utf8(e.path);
            std::fprintf(stderr, "[editor] instantiate requested: %s\n", instantiate.c_str());
        }
    }
    if (ImGui::BeginPopupContextItem("##asset_menu")) {
        if (e.is_dir && ImGui::MenuItem(ui::label(icon::folder_open, "Open").c_str())) {
            open_folder = true;
        }
        if (type.kind == AssetKind::model &&
            ImGui::MenuItem(ui::label(icon::plus, "Place in Scene").c_str())) {
            instantiate = utf8(e.path);
        }
        if (ImGui::MenuItem(ui::label(icon::copy, "Copy Path").c_str())) {
            ImGui::SetClipboardText(utf8(e.path).c_str());
        }
        if (ImGui::MenuItem(ui::label(icon::folder_tree, "Show in File Manager").c_str())) {
            const fs::path dir = e.is_dir ? e.path : e.path.parent_path();
            (void)SDL_OpenURL(("file://" + utf8(dir)).c_str());
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(e.name.c_str());
        ui::text_secondary(e.is_dir ? std::string("Folder - double-click to open")
                                    : std::string(type.label) + "  " + human_size(e.size) +
                                          (type.kind == AssetKind::model
                                               ? "  - double-click or drag to the viewport"
                                               : ""));
        ImGui::EndTooltip();
    }
    return open_folder;
}

void draw_grid(AssetBrowserState& s, const fs::path& root, std::string& instantiate) {
    const float tile = ui::scaled(s.tile);
    const float gap = ui::scaled(ui::Spacing::xs);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + gap) / (tile + gap)));
    const float icon_h = tile * 0.55F;
    const float label_h = ImGui::GetTextLineHeight() * 2.0F + ui::scaled(4.0F);
    const ui::Theme& t = ui::tokens();
    fs::path go;
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        const Entry& e = s.entries[i];
        const AssetTypeInfo type = asset_type(e.path, e.is_dir);
        ImGui::PushID(static_cast<int>(i));
        if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine(0.0F, gap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", ImVec2(tile, icon_h + label_h));
        const bool hovered = ImGui::IsItemHovered();
        if (item_behaviour(e, type, instantiate)) go = e.path.lexically_relative(root);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered) {
            dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + icon_h + label_h), ui::to_u32(t.bg3),
                              ui::scaled(ui::Radius::md));
        }
        {
            ui::FontScope big(ui::FontRole::icon_large);
            const char* glyph = ui::ic(type.icon);
            const ImVec2 gs = ImGui::CalcTextSize(glyph);
            const float scale = 1.6F;
            const ImVec2 at(p.x + (tile - gs.x * scale) * 0.5F, p.y + (icon_h - gs.y * scale) * 0.5F);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, at,
                        ui::to_u32(kind_tint(type.kind)), glyph);
        }
        // Name: two centred lines, ellipsised.
        const float pad = ui::scaled(4.0F);
        const float wrap = tile - pad * 2.0F;
        std::string name = e.name;
        const ImVec2 full = ImGui::CalcTextSize(name.c_str(), nullptr, false, wrap);
        if (full.y > ImGui::GetTextLineHeight() * 2.0F + 1.0F) {
            while (name.size() > 1 &&
                   ImGui::CalcTextSize((name + "...").c_str(), nullptr, false, wrap).y >
                       ImGui::GetTextLineHeight() * 2.0F + 1.0F) {
                name.pop_back();
            }
            name += "...";
        }
        const ImVec2 ns = ImGui::CalcTextSize(name.c_str(), nullptr, false, wrap);
        dl->AddText(nullptr, 0.0F, ImVec2(p.x + (tile - ns.x) * 0.5F, p.y + icon_h), ui::to_u32(t.text),
                    name.c_str(), nullptr, wrap);
        ImGui::PopID();
    }
    if (!go.empty()) navigate(s, go);
}

void draw_list(AssetBrowserState& s, const fs::path& root, std::string& instantiate) {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##list", 3, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0F);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.2F);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableHeadersRow();
    fs::path go;
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        const Entry& e = s.entries[i];
        const AssetTypeInfo type = asset_type(e.path, e.is_dir);
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ui::text_colored(kind_tint(type.kind), ui::ic(type.icon));
        ImGui::SameLine();
        ImGui::Selectable(e.name.c_str(), false,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                              ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0.0F, ImGui::GetFrameHeight() - ImGui::GetStyle().CellPadding.y * 2.0F));
        if (item_behaviour(e, type, instantiate)) go = e.path.lexically_relative(root);
        ImGui::TableSetColumnIndex(1);
        ui::text_secondary(e.is_dir ? "Folder" : type.label);
        ImGui::TableSetColumnIndex(2);
        if (!e.is_dir) {
            ui::FontScope mono(ui::FontRole::mono);
            ui::text_secondary(human_size(e.size));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!go.empty()) navigate(s, go);
}

void breadcrumbs(AssetBrowserState& s) {
    const ui::Theme& t = ui::tokens();
    if (ui::icon_button("home", icon::house, "Project asset roots")) navigate(s, {});
    fs::path acc;
    fs::path go;
    bool any = false;
    for (const fs::path& part : s.dir) {
        acc /= part;
        ImGui::SameLine(0.0F, ui::scaled(2.0F));
        ImGui::AlignTextToFramePadding();
        ui::text_colored(t.text_disabled, ui::ic(icon::chevron_right));
        ImGui::SameLine(0.0F, ui::scaled(2.0F));
        const std::string label = utf8(part) + "##crumb" + utf8(acc);
        if (ui::button(label.c_str(), ui::ButtonKind::ghost)) go = acc;
        any = true;
    }
    if (any && !go.empty() && go != s.dir) navigate(s, go);
}

} // namespace

AssetTypeInfo asset_type(const fs::path& path, bool is_directory) {
    if (is_directory) return {AssetKind::folder, icon::folder, "Folder"};
    const std::string e = lower_ext(path);
    if (e == ".gltf" || e == ".glb") return {AssetKind::model, icon::file_box, "Model"};
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".hdr" || e == ".ktx2" || e == ".exr") {
        return {AssetKind::texture, icon::file_image, "Texture"};
    }
    if (e == ".slang") return {AssetKind::shader, icon::file_code, "Shader"};
    if (e == ".json") return {AssetKind::scene, icon::file_json, "Scene / data"};
    if (e == ".wav" || e == ".flac" || e == ".mp3" || e == ".ogg") {
        return {AssetKind::audio, icon::file_audio, "Audio"};
    }
    if (e == ".ttf" || e == ".otf") return {AssetKind::font, icon::type, "Font"};
    if (e == ".txt" || e == ".md") return {AssetKind::text, icon::file_text, "Text"};
    return {AssetKind::other, icon::file, "File"};
}

void draw_asset_browser(const std::string& project_root, std::string& instantiate_request,
                        AssetBrowserState& state, const char* window_name, bool* open) {
    if (open != nullptr && !*open) return;
    if (!ImGui::Begin(window_name, open)) {
        ImGui::End();
        return;
    }
    const fs::path root(std::u8string(project_root.begin(), project_root.end()));
    std::error_code ec;
    if (!state.dir.empty() && !fs::is_directory(root / state.dir, ec)) navigate(state, {});

    // Toolbar: breadcrumbs | search | grid/list | refresh.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float search_w = ui::scaled(196.0F);
    const float right = search_w + ImGui::GetFrameHeight() * 3.0F + ui::scaled(2.0F) * 2.0F +
                        style.ItemSpacing.x + style.WindowPadding.x;
    ImGui::BeginGroup();
    breadcrumbs(state);
    ImGui::EndGroup();
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - right));
    if (ui::search_field("asset_search", state.filter, "Search assets", search_w)) {
        state.dirty = true;
    }
    ImGui::SameLine();
    ui::begin_toolbar();
    if (ui::toggle_button("grid", icon::layout_grid, state.grid, "Grid view")) state.grid = true;
    ImGui::SameLine();
    if (ui::toggle_button("list", icon::list, !state.grid, "List view")) state.grid = false;
    ImGui::SameLine();
    if (ui::icon_button("refresh", icon::refresh_cw, "Rescan the folder")) state.dirty = true;
    ui::end_toolbar();
    ImGui::Separator();

    refresh(state, root);
    ImGui::BeginChild("##content", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
    if (state.entries.empty()) {
        if (!state.filter.empty()) {
            (void)ui::empty_state(icon::search, "No matching assets",
                                  "The search covers this folder and every folder below it.");
        } else if (state.dir.empty()) {
            (void)ui::empty_state(icon::folder, "No asset folders",
                                  "Create assets/ (or sample-assets/) in the project root.");
        } else {
            (void)ui::empty_state(icon::folder_open, "This folder is empty",
                                  "Add files on disk; the view rescans every few seconds.");
        }
    } else if (state.grid) {
        draw_grid(state, root, instantiate_request);
    } else {
        draw_list(state, root, instantiate_request);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace engine::editor
