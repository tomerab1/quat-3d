#include "asset_browser.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

namespace engine::editor {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool is_gltf(const fs::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".gltf" || ext == ".glb";
}

// Short type tag shown before the file name (thumbnails come later).
[[nodiscard]] const char* file_tag(const fs::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".gltf" || ext == ".glb") return "[mdl]";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr") return "[tex]";
    if (ext == ".slang") return "[shd]";
    if (ext == ".bin") return "[bin]";
    return "[   ]";
}

void draw_directory(const fs::path& dir, std::string& instantiate_request) {
    // Sorted: directories first, then files, both alphabetical.
    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  const bool da = a.is_directory();
                  const bool db = b.is_directory();
                  if (da != db) return da;
                  return a.path().filename() < b.path().filename();
              });

    for (const auto& entry : entries) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == '.') continue;

        if (entry.is_directory()) {
            if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                draw_directory(entry.path(), instantiate_request);
                ImGui::TreePop();
            }
            continue;
        }

        const std::string label = std::string(file_tag(entry.path())) + " " + name;
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (is_gltf(entry.path())) {
            const auto request = [&]() {
                instantiate_request = entry.path().string();
                std::fprintf(stderr, "[editor] instantiate requested: %s\n",
                             instantiate_request.c_str());
            };
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                request();
            }
            if (ImGui::BeginDragDropSource()) {
                const std::string path = entry.path().string();
                ImGui::SetDragDropPayload(asset_drag_payload, path.c_str(), path.size() + 1);
                ImGui::Text("instantiate %s", name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Instantiate")) request();
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "double-click, right-click, or drag onto the viewport to instantiate");
            }
        }
    }
}

} // namespace

void draw_asset_browser(const std::string& project_root, std::string& instantiate_request) {
    if (!ImGui::Begin("Assets")) {
        ImGui::End();
        return;
    }
    bool any = false;
    for (const char* sub : {"assets", "sample-assets"}) {
        const fs::path root = fs::path(project_root) / sub;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        any = true;
        if (ImGui::TreeNodeEx(sub, ImGuiTreeNodeFlags_DefaultOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth)) {
            draw_directory(root, instantiate_request);
            ImGui::TreePop();
        }
    }
    if (!any) {
        ImGui::TextUnformatted("(no asset directories found)");
    }
    ImGui::End();
}

} // namespace engine::editor
