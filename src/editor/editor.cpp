#include "engine/editor/editor.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include <imgui.h>

#include <ImGuizmo.h> // must follow imgui.h

#include "behavior_panel.hpp"
#include "shell/editor_shell.hpp"
#include "vendor/imgui_impl_sdl3.h"

namespace engine::editor {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

std::expected<EditorLayer, core::Error> EditorLayer::create(SDL_Window* window,
                                                            const std::string& project_root) {
    if (window == nullptr) {
        return fail("editor: null window");
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        return fail("editor: an ImGui context already exists");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // the shell points it at the project's layout file
    io.BackendRendererName = "quat3d (descriptor buffer)";

    if (!ImGui_ImplSDL3_InitForOther(window)) {
        ImGui::DestroyContext();
        return fail("editor: ImGui_ImplSDL3_Init failed");
    }

    EditorLayer out;
    out.initialized_ = true;
    // Fonts, theme, commands, layout persistence (ROADMAP E8).
    out.shell_ = std::make_unique<shell::EditorShell>(window, std::filesystem::path(project_root));
    return out;
}

EditorLayer::EditorLayer() = default;

EditorLayer::~EditorLayer() {
    destroy();
}

EditorLayer::EditorLayer(EditorLayer&& other) noexcept {
    *this = std::move(other);
}

EditorLayer& EditorLayer::operator=(EditorLayer&& other) noexcept {
    if (this != &other) {
        destroy();
        initialized_ = std::exchange(other.initialized_, false);
        font_atlas_changed_ = other.font_atlas_changed_;
        shell_ = std::move(other.shell_);
    }
    return *this;
}

void EditorLayer::destroy() noexcept {
    if (initialized_) {
        // Save the layout + editor preferences while the shell (which the
        // ini handler writes from) is alive; DestroyContext must not save
        // again. The shell's node-editor context needs ImGui alive.
        ImGuiIO& io = ImGui::GetIO();
        if (io.IniFilename != nullptr) ImGui::SaveIniSettingsToDisk(io.IniFilename);
        io.IniFilename = nullptr;
        shell_.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        initialized_ = false;
    }
}

void EditorLayer::process_event(const SDL_Event& event) {
    if (initialized_) ImGui_ImplSDL3_ProcessEvent(&event);
}

void EditorLayer::set_theme(std::string_view name) {
    if (shell_ == nullptr) return;
    shell_->force_theme(name == "light" ? ui::ThemeId::light : ui::ThemeId::dark);
}

bool EditorLayer::take_font_atlas_changed() {
    return std::exchange(font_atlas_changed_, false);
}

bool EditorLayer::wants_mouse() const {
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool EditorLayer::wants_keyboard() const {
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

bool EditorLayer::wants_text_input() const {
    return initialized_ && ImGui::GetIO().WantTextInput;
}

std::uint32_t EditorLayer::viewport_width() const {
    return shell_ != nullptr ? shell_->viewport_width : 0;
}
std::uint32_t EditorLayer::viewport_height() const {
    return shell_ != nullptr ? shell_->viewport_height : 0;
}
bool EditorLayer::viewport_hovered() const {
    return shell_ != nullptr && shell_->viewport_hovered;
}
std::array<float, 4> EditorLayer::viewport_rect() const {
    if (shell_ == nullptr) return {};
    return {shell_->viewport_rect[0], shell_->viewport_rect[1], shell_->viewport_rect[2],
            shell_->viewport_rect[3]};
}
entt::entity EditorLayer::selected() const {
    return shell_ != nullptr ? shell_->selected : entt::null;
}
void EditorLayer::set_selected(entt::entity entity) {
    if (shell_ != nullptr) shell_->selected = entity;
}
void EditorLayer::focus_panel(std::string title) {
    if (shell_ != nullptr) shell_->focus_panel(std::move(title));
}
void EditorLayer::notify(NoticeKind kind, std::string text) {
    if (shell_ != nullptr) shell_->notify(kind, std::move(text));
}
void EditorLayer::use_saved_layout(bool use) {
    if (shell_ != nullptr) shell_->set_use_saved_layout(use);
}

void EditorLayer::begin_frame() {
    if (!initialized_) return;
    // Theme and display-scale changes apply before the frame starts.
    if (shell_->before_new_frame()) font_atlas_changed_ = true;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void EditorLayer::build_ui(const EditorContext& ctx) {
    if (!initialized_) return;
    shell_->draw(ctx);
}

const ImDrawData* EditorLayer::end_frame() {
    if (!initialized_) return nullptr;
    ImGui::Render();
    return ImGui::GetDrawData();
}

} // namespace engine::editor
