// Duplicate-ID sweep over the real panels (ROADMAP E8.3): the Hierarchy (a
// small tree), the Inspector on an entity carrying many components, the
// Console and the Assets browser draw side by side in a renderer-less ImGui
// context while the mouse visits every point of a grid. ImGui reports an ID
// submitted twice when it is hovered (ConfigDebugHighlightIdConflicts), and
// its recoverable errors (unbalanced Push/Pop, Begin/End) are counted.

#ifdef ENGINE_EDITOR

#include <filesystem>
#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include "editor/asset_browser.hpp"
#include "editor/inspector.hpp"
#include "editor/panels/console.hpp"
#include "editor/panels/inspector_parts.hpp"
#include "editor/scene_hierarchy.hpp"
#include "editor/ui/fonts.hpp"
#include "editor/ui/theme.hpp"
#include "editor/ui/toasts.hpp"
#include "engine/core/log.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace ed = engine::editor;
namespace ui = engine::editor::ui;
namespace s = engine::scene;
using nlohmann::json;

constexpr float k_panel_w = 400.0F;
constexpr float k_panel_h = 1100.0F;

int         g_errors = 0;
std::string g_last_error;

struct Panels {
    s::Scene                   scene;
    entt::entity               selected = entt::null;
    ed::HierarchyState         hierarchy;
    ed::panels::InspectorState inspector;
    ed::panels::ConsoleState   console;
    ed::AssetBrowserState      assets;
    std::string                instantiate;
    std::string                root = QUAT_PROJECT_ROOT;
};

void place(int column) {
    ImGui::SetNextWindowPos(ImVec2(k_panel_w * static_cast<float>(column), 0.0F));
    ImGui::SetNextWindowSize(ImVec2(k_panel_w, k_panel_h));
}

void draw(Panels& p) {
    place(0);
    ed::draw_scene_hierarchy(p.scene, p.selected, p.hierarchy, glm::vec3(0.0F), "Hierarchy",
                             nullptr);
    place(1);
    ed::draw_inspector(p.scene, p.selected, p.inspector);
    place(2);
    ed::panels::draw_console(p.console, "Console", nullptr);
    place(3);
    ed::draw_asset_browser(p.root, p.instantiate, p.assets, "Assets", nullptr);
}

class EditorPanelSweep : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(k_panel_w * 4.0F, k_panel_h);
        io.DeltaTime = 1.0F / 60.0F;
        io.ConfigDebugHighlightIdConflicts = true;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        ctx_->ErrorCallback = [](ImGuiContext*, void*, const char* msg) {
            ++g_errors;
            g_last_error = msg != nullptr ? msg : "";
        };
        g_errors = 0;
        g_last_error.clear();
        ui::toasts().clear();
        (void)ui::load_fonts(*io.Fonts, std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts",
                             ui::DpiScale{1.0F, 1.0F});
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    void TearDown() override {
        ui::toasts().clear();
        ui::set_fonts({});
        ImGui::DestroyContext(ctx_);
    }

    ImGuiID frame(Panels& p, ImVec2 mouse) {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        const ImGuiID conflict = ctx_->DebugDrawIdConflictsId;
        draw(p);
        ImGui::Render();
        return conflict;
    }

    ImGuiContext* ctx_ = nullptr;
};

TEST_F(EditorPanelSweep, RealPanelsHaveNoDuplicateIds) {
    engine::core::log_message(engine::core::LogLevel::info, "test", "panel sweep: a console line");
    engine::core::log_message(engine::core::LogLevel::warn, "test", "panel sweep: a warning");

    Panels p;
    entt::registry& r = p.scene.registry();
    const entt::entity group = p.scene.create_entity("Props");
    const entt::entity lamp = p.scene.create_entity("Lamp");
    ASSERT_TRUE(p.scene.set_parent(lamp, group));
    ASSERT_OK(s::apply_component(r, lamp, "point_light", json::object(), false));
    (void)p.scene.create_entity("Crate");
    // The inspected entity: one section per kind of editor.
    const entt::entity e = p.scene.create_entity("Everything");
    for (const char* c : {"primitive", "point_light", "camera", "collider", "rigidbody",
                          "audio_source", "audio_listener", "nav_agent", "patrol", "behavior",
                          "grass", "dir_light"}) {
        ASSERT_OK(s::apply_component(r, e, c, json::object(), false)) << c;
    }
    ASSERT_OK(s::apply_component(r, e, "material", json{{"roughness", 0.3}}, false));
    p.selected = e;
    // Open every folder level the browser shows first.
    p.assets.dir = "assets";

    for (const ui::Theme* theme : {&ui::dark_theme(), &ui::light_theme()}) {
        ui::apply_theme(*theme, 1.0F);
        for (int i = 0; i < 3; ++i) (void)frame(p, ImVec2(-1.0F, -1.0F));
        for (float y = 2.0F; y < k_panel_h; y += 9.0F) {
            for (float x = 6.0F; x < k_panel_w * 4.0F; x += 46.0F) {
                for (int k = 0; k < 2; ++k) {
                    const ImGuiID conflict = frame(p, ImVec2(x, y));
                    ASSERT_EQ(conflict, 0U)
                        << theme->name << ": duplicate ImGui ID under (" << x << ", " << y << ")";
                }
            }
        }
        EXPECT_EQ(g_errors, 0) << theme->name << ": " << g_last_error;
    }
    EXPECT_EQ(p.selected, e) << "hovering never changes the selection";
}

} // namespace

#endif // ENGINE_EDITOR
