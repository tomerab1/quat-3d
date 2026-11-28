// Widget kit: asset-path fields (drop target + browse popup).

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "fonts.hpp"
#include "icons.hpp"
#include "metrics.hpp"
#include "widgets.hpp"

namespace engine::editor::ui {

namespace {

namespace fs = std::filesystem;

// Browse popup state: one popup is open at a time, so one cache suffices.
struct BrowseCache {
    std::vector<std::string> files; // project-relative, '/'-separated
    std::string              filter;
};
BrowseCache g_browse;

constexpr std::size_t k_max_listed_files = 4000;

[[nodiscard]] std::string to_utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return {s.begin(), s.end()};
}

[[nodiscard]] bool has_extension(const fs::path& p, std::span<const std::string_view> exts) {
    if (exts.empty()) return true;
    std::string ext = to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

// Project files under assets/ and sample-assets/ with a matching extension.
void scan(const fs::path& root, std::span<const std::string_view> exts,
          std::vector<std::string>& out) {
    out.clear();
    for (const char* sub : {"assets", "sample-assets", "scenes"}) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root / sub,
                                            fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (out.size() >= k_max_listed_files) return;
            if (!it->is_regular_file(ec) || !has_extension(it->path(), exts)) continue;
            out.push_back(to_utf8(it->path().lexically_relative(root)));
        }
    }
    std::sort(out.begin(), out.end());
}

// An absolute path dropped from the asset browser, made project-relative
// when it lies under the root.
[[nodiscard]] std::string relative_to_root(const fs::path& root, const std::string& dropped) {
    const fs::path p(std::u8string(dropped.begin(), dropped.end()));
    const fs::path rel = p.lexically_relative(root);
    if (!rel.empty() && *rel.begin() != "..") return to_utf8(rel);
    return dropped;
}

} // namespace

bool prop_asset(const char* text, std::string& path, const AssetSpec& spec,
                const PropOpts<std::string>& opts) {
    const bool has_reset = opts.reset.has_value() && !opts.read_only;
    const bool modified = has_reset && *opts.reset != path;
    prop_begin(text, opts.tooltip, has_reset, modified);
    bool changed = false;
    ImGui::BeginDisabled(opts.read_only);

    // [path field][browse]: the field shrinks to leave room for the button.
    const float browse_w = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(ImGui::CalcItemWidth() - browse_w - spacing, scaled(40.0F)));
    std::string draft = path;
    const ImGuiInputTextFlags flags =
        spec.commit_on_enter ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
    if (ImGui::InputTextWithHint("##path", "none", &draft, flags) ||
        (spec.commit_on_enter && ImGui::IsItemDeactivatedAfterEdit())) {
        if (draft != path) {
            path = std::move(draft);
            changed = true;
        }
    }
    if (!path.empty()) item_tooltip(path.c_str(), spec.commit_on_enter ? "Enter to apply" : nullptr);
    // Files dragged from the asset browser.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(k_asset_payload, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            const std::string dropped(static_cast<const char*>(payload->Data));
            const bool ok = has_extension(fs::path(std::u8string(dropped.begin(), dropped.end())),
                                          spec.extensions);
            if (payload->IsDelivery() && ok) {
                path = relative_to_root(spec.root, dropped);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine(0.0F, spacing);
    if (icon_button("browse", icon::folder_open, "Browse project files")) {
        g_browse.filter.clear();
        scan(spec.root, spec.extensions, g_browse.files);
        ImGui::OpenPopup("##asset_browse");
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(320.0F), 0.0F),
                                        ImVec2(scaled(520.0F), scaled(360.0F)));
    if (ImGui::BeginPopup("##asset_browse")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        (void)search_field("filter", g_browse.filter, "Filter files");
        ImGui::BeginChild("##files", ImVec2(0.0F, scaled(260.0F)));
        int shown = 0;
        for (const std::string& f : g_browse.files) {
            if (!matches_filter(f, g_browse.filter)) continue;
            ++shown;
            if (ImGui::Selectable(label(icon::file, f).c_str(), f == path)) {
                path = f;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (shown == 0) text_colored(tokens().text_disabled, "No matching files");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (prop_end(has_reset, modified)) {
        path = *opts.reset;
        changed = true;
    }
    return changed;
}

} // namespace engine::editor::ui
