// Widget kit smoke test (ROADMAP E8.1): an ImGui context without a renderer
// (NewFrame / Render only, no GPU) draws every kit widget in both themes,
// with the real fonts and with none. The mouse sweeps the window so ImGui's
// duplicate-ID detector sees every interactive item; ImGui's recoverable
// errors (unbalanced Push/Pop, Begin/End, ...) are counted through its error
// callback instead of asserting.

#ifdef ENGINE_EDITOR

#include <filesystem>
#include <string>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "editor/ui/fonts.hpp"
#include "editor/ui/icons.hpp"
#include "editor/ui/metrics.hpp"
#include "editor/ui/theme.hpp"
#include "editor/ui/toasts.hpp"
#include "editor/ui/widgets.hpp"

namespace {

namespace ui = engine::editor::ui;
namespace icon = engine::editor::icon;

enum class Shape { cube, sphere, plane };

// Every widget of the kit, once.
struct KitState {
    float       f = 1.5F;
    float       angle = 0.3F;
    int         i = 3;
    std::uint32_t u = 7;
    bool        b = true;
    bool        enabled = true;
    glm::vec2   v2{1.0F, 2.0F};
    glm::vec3   v3{1.0F, 2.0F, 3.0F};
    glm::vec4   v4{1.0F, 2.0F, 3.0F, 4.0F};
    glm::vec2   range{0.5F, 2.0F};
    glm::vec3   c3{1.0F, 0.5F, 0.25F};
    glm::vec4   c4{1.0F, 0.5F, 0.25F, 0.5F};
    std::string text = "metal_sphere";
    std::string asset = "assets/meshes/a.glb";
    std::string search;
    Shape       shape = Shape::sphere;
    bool        toggled = false;
};

void draw_kit(KitState& s) {
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(520.0F, 1400.0F));
    ImGui::Begin("kit", nullptr, ImGuiWindowFlags_NoSavedSettings);

    ui::begin_toolbar();
    (void)ui::icon_button("play", icon::play, "Play", "Ctrl+P");
    ImGui::SameLine();
    (void)ui::toggle_button("snap", icon::magnet, s.toggled, "Snap", "Ctrl+G");
    ui::toolbar_separator();
    (void)ui::icon_button("disabled", icon::pause, "Pause", nullptr, /*enabled=*/false);
    ui::end_toolbar();

    (void)ui::search_field("search", s.search, "Search entities");
    (void)ui::button("Secondary");
    ImGui::SameLine();
    (void)ui::button("Primary", ui::ButtonKind::primary);
    ImGui::SameLine();
    (void)ui::button("Danger", ui::ButtonKind::danger);
    ImGui::SameLine();
    (void)ui::button("Ghost", ui::ButtonKind::ghost);
    ui::text_secondary("secondary text");
    ui::text_colored(ui::tokens().error, "error text");
    ImGui::TextUnformatted(ui::label(icon::info, "labelled").c_str());
    ui::item_tooltip("tooltip", "F1");

    const ui::ComponentHeader h = ui::component_header({.id = "transform",
                                                        .icon = icon::move,
                                                        .title = "Transform",
                                                        .removable = false,
                                                        .tooltip = "Local TRS"});
    if (h.open && ui::begin_property_grid("transform_grid")) {
        (void)ui::prop_vec3("Position", s.v3, {.speed = 0.05F}, {.reset = glm::vec3(0.0F)});
        (void)ui::prop_vec2("Size", s.v2);
        (void)ui::prop_vec4("Quaternion", s.v4, {}, {.tooltip = "raw"});
        ui::end_property_grid();
    }
    const ui::ComponentHeader h2 = ui::component_header({.id = "light",
                                                         .icon = icon::lightbulb,
                                                         .title = "Point Light",
                                                         .enabled = &s.enabled,
                                                         .can_paste = true});
    if (h2.open && ui::begin_property_grid("light_grid")) {
        (void)ui::prop_float("Intensity", s.f, {.speed = 0.05F, .min = 0.0F, .max = 100.0F},
                             {.reset = 1.0F, .tooltip = "candela"});
        (void)ui::prop_float("Slider", s.f, {.min = 0.0F, .max = 10.0F, .slider = true});
        (void)ui::prop_angle("Angle", s.angle);
        (void)ui::prop_int("Count", s.i, {.min = 0, .max = 10}, {.reset = 1});
        (void)ui::prop_u32("Seed", s.u);
        (void)ui::prop_bool("Cast shadows", s.b, {.reset = false});
        (void)ui::prop_range("Scale range", s.range);
        (void)ui::prop_color3("Colour", s.c3, {.reset = glm::vec3(1.0F)});
        (void)ui::prop_color4("Tint", s.c4, {}, /*hdr=*/true);
        ui::prop_separator("Advanced");
        (void)ui::prop_text("Name", s.text);
        (void)ui::prop_text("Committed", s.text, {}, /*commit_on_enter=*/true);
        ui::prop_value("Vertices", "1617", "read-only", /*mono=*/true);
        static constexpr ui::EnumItem shapes[] = {
            {static_cast<int>(Shape::cube), "Cube", "six faces"},
            {static_cast<int>(Shape::sphere), "Sphere", nullptr},
            {static_cast<int>(Shape::plane), "Plane", nullptr},
        };
        (void)ui::prop_enum("Shape", s.shape, shapes, {.reset = Shape::cube});
        static constexpr std::string_view exts[] = {".gltf", ".glb"};
        (void)ui::prop_asset("Model", s.asset,
                             {.root = std::filesystem::path(QUAT_PROJECT_ROOT), .extensions = exts},
                             {.reset = std::string()});
        (void)ui::prop_text("Read-only", s.text, {.read_only = true});
        ui::end_property_grid();
    }
    if (ui::section_header("Section")) ImGui::TextUnformatted("section body");
    ImGui::BeginChild("empty", ImVec2(0.0F, 220.0F), ImGuiChildFlags_Borders);
    (void)ui::empty_state(icon::mouse_pointer_2, "Nothing selected",
                          "Select an entity in the Hierarchy or click it in the viewport.",
                          "Create entity");
    ImGui::EndChild();
    ImGui::End();
    ui::draw_toasts(1.0F / 60.0F, ImVec2(1600.0F, 1400.0F));
}

int g_errors = 0;
std::string g_last_error;

class EditorUiWidgets : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1400.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        // On by default only in debug builds of ImGui: the sweep relies on it.
        io.ConfigDebugHighlightIdConflicts = true;
        ctx_->ErrorCallback = [](ImGuiContext*, void*, const char* msg) {
            ++g_errors;
            g_last_error = msg != nullptr ? msg : "";
        };
        g_errors = 0;
        g_last_error.clear();
        ui::toasts().clear();
    }
    void TearDown() override {
        ui::toasts().clear();
        ui::set_fonts({});
        ImGui::DestroyContext(ctx_);
    }

    // Builds the atlas the way the renderer would (legacy upload path).
    static void build_atlas() {
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ASSERT_NE(pixels, nullptr);
    }

    // One frame with the mouse at `mouse`; returns the ID ImGui flagged as
    // submitted more than once by the previous frame (0 = none).
    ImGuiID frame(KitState& s, ImVec2 mouse) {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        const ImGuiID conflict = ctx_->DebugDrawIdConflictsId;
        draw_kit(s);
        ImGui::Render();
        return conflict;
    }

    void sweep(KitState& s) {
        for (float y = 2.0F; y < 1400.0F; y += 7.0F) {
            for (const float x : {20.0F, 120.0F, 200.0F, 260.0F, 330.0F, 400.0F, 470.0F, 505.0F}) {
                for (int k = 0; k < 2; ++k) {
                    const ImGuiID conflict = frame(s, ImVec2(x, y));
                    ASSERT_EQ(conflict, 0U) << "duplicate ImGui ID under (" << x << ", " << y << ")";
                }
            }
        }
    }

    ImGuiContext* ctx_ = nullptr;
};

TEST_F(EditorUiWidgets, EveryWidgetDrawsInBothThemesWithoutConflicts) {
    const ui::FontSet fonts = ui::load_fonts(*ImGui::GetIO().Fonts,
                                             std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts",
                                             ui::DpiScale{1.0F, 1.0F});
    EXPECT_TRUE(fonts.inter);
    EXPECT_TRUE(fonts.mono);
    EXPECT_TRUE(fonts.icons);
    build_atlas();
    ui::notify(ui::ToastKind::success, "Scene saved");
    ui::notify(ui::ToastKind::error, "Load failed: a long message that has to wrap inside the toast");

    KitState s;
    for (const ui::Theme* theme : {&ui::dark_theme(), &ui::light_theme()}) {
        ui::apply_theme(*theme, 1.0F);
        EXPECT_EQ(&ui::tokens(), theme);
        for (int i = 0; i < 3; ++i) (void)frame(s, ImVec2(-1.0F, -1.0F));
        sweep(s);
        EXPECT_EQ(g_errors, 0) << theme->name << ": " << g_last_error;
    }
}

TEST_F(EditorUiWidgets, MissingFontsFallBackToTheBuiltInFont) {
    const ui::FontSet fonts = ui::load_fonts(*ImGui::GetIO().Fonts, "/nonexistent/fonts",
                                             ui::DpiScale{1.5F, 1.0F});
    EXPECT_FALSE(fonts.inter);
    EXPECT_FALSE(fonts.icons);
    for (const ImFont* f : fonts.fonts) EXPECT_NE(f, nullptr) << "every role has a font";
    EXPECT_STREQ(ui::ic(icon::play), "") << "no icon font: icons collapse, no tofu";
    EXPECT_EQ(ui::label(icon::play, "Play"), "Play");
    build_atlas();
    ui::apply_theme(ui::dark_theme(), 1.5F);
    KitState s;
    for (int i = 0; i < 4; ++i) (void)frame(s, ImVec2(300.0F, 200.0F));
    EXPECT_EQ(g_errors, 0) << g_last_error;
}

TEST_F(EditorUiWidgets, ScaledStyleFollowsTheDpiScale) {
    (void)ui::load_fonts(*ImGui::GetIO().Fonts, std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts",
                         ui::DpiScale{2.0F, 1.0F});
    ui::apply_theme(ui::dark_theme(), 1.0F);
    const float padding_1x = ImGui::GetStyle().FramePadding.x;
    ui::apply_theme(ui::dark_theme(), 2.0F);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().FramePadding.x, padding_1x * 2.0F);
    EXPECT_FLOAT_EQ(ui::ui_scale(), 2.0F);
    EXPECT_FLOAT_EQ(ui::scaled(12.0F), 24.0F);
    EXPECT_FLOAT_EQ(ui::font(ui::FontRole::body)->LegacySize, ui::font_px(ui::TypeScale::body, 2.0F));
    ui::apply_theme(ui::dark_theme(), 1.0F); // leave the global scale as other tests expect
}

} // namespace

#endif // ENGINE_EDITOR
