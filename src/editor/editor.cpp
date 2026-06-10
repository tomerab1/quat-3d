#include "engine/editor/editor.hpp"

#include <string>
#include <utility>

#include <imgui.h>

#include "vendor/imgui_impl_sdl3.h"

namespace engine::editor {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

std::expected<EditorLayer, core::Error> EditorLayer::create(SDL_Window* window) {
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
    io.IniFilename = nullptr; // panel layout persistence arrives with the real layout
    io.BackendRendererName = "quat3d (descriptor buffer)";
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOther(window)) {
        ImGui::DestroyContext();
        return fail("editor: ImGui_ImplSDL3_Init failed");
    }

    EditorLayer out;
    out.initialized_ = true;
    return out;
}

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
        show_stats_ = other.show_stats_;
        show_demo_ = other.show_demo_;
    }
    return *this;
}

void EditorLayer::destroy() noexcept {
    if (initialized_) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        initialized_ = false;
    }
}

void EditorLayer::process_event(const SDL_Event& event) {
    if (initialized_) ImGui_ImplSDL3_ProcessEvent(&event);
}

bool EditorLayer::wants_mouse() const {
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool EditorLayer::wants_keyboard() const {
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

void EditorLayer::begin_frame() {
    if (!initialized_) return;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void EditorLayer::build_ui(const FrameStats& stats) {
    if (!initialized_) return;

    // Fullscreen dockspace; the central node stays transparent so the scene
    // renders through until the offscreen viewport panel lands (9.2).
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Stats", nullptr, &show_stats_);
            ImGui::MenuItem("ImGui Demo", nullptr, &show_demo_);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (show_stats_) {
        if (ImGui::Begin("Stats", &show_stats_)) {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("%.1f fps (%.2f ms)", io.Framerate,
                        io.Framerate > 0.0F ? 1000.0F / io.Framerate : 0.0F);
            ImGui::Text("frame: %.2f ms", stats.frame_ms);
            ImGui::Text("draws: %d", stats.draw_count);
            ImGui::Text("entities: %d", stats.entity_count);
        }
        ImGui::End();
    }
    if (show_demo_) {
        ImGui::ShowDemoWindow(&show_demo_);
    }
}

const ImDrawData* EditorLayer::end_frame() {
    if (!initialized_) return nullptr;
    ImGui::Render();
    return ImGui::GetDrawData();
}

} // namespace engine::editor
