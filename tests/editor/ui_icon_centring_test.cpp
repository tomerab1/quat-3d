// Icon buttons centre the glyph's ink, not its advance box (ROADMAP E8.1):
// the Lucide merge sets a minimum advance and a vertical offset tuned for
// icon + text labels, so a glyph passed as an ImGui::Button label sits off
// centre in a square button. Builds the real atlas headlessly, draws the
// toolbar's icon buttons and checks the drawn glyph quad's centre against
// the button's centre at several UI scales.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "editor/ui/fonts.hpp"
#include "editor/ui/icons.hpp"
#include "editor/ui/theme.hpp"
#include "editor/ui/widgets.hpp"

namespace {

namespace ui = engine::editor::ui;
namespace icon = engine::editor::icon;

// The main toolbar's icons (play controls, transform tools, snapping).
constexpr const char* k_toolbar_icons[] = {
    icon::play,  icon::pause,     icon::step_forward, icon::square, icon::move,
    icon::rotate_3d, icon::scale_3d, icon::magnet,  icon::globe,
};

class EditorIconCentring : public ::testing::TestWithParam<float> {
protected:
    void SetUp() override {
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(2000.0F, 400.0F);
        io.DeltaTime = 1.0F / 60.0F;
        const ui::FontSet fonts =
            ui::load_fonts(*io.Fonts, std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts",
                           ui::DpiScale{GetParam(), 1.0F});
        have_icons_ = fonts.icons;
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ui::apply_theme(ui::dark_theme(), GetParam());
    }
    void TearDown() override {
        ui::set_fonts({});
        ui::apply_theme(ui::dark_theme(), 1.0F);
        ImGui::DestroyContext(ctx_);
    }

    ImGuiContext* ctx_ = nullptr;
    bool          have_icons_ = false;
};

TEST_P(EditorIconCentring, InkCentreMatchesTheButtonCentre) {
    ASSERT_TRUE(have_icons_) << "the Lucide font ships in assets/fonts";
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10.3F, 7.6F)); // fractional on purpose
    // An explicit size: a new auto-sized window clips (culls) its text on
    // the first frame.
    ImGui::SetNextWindowSize(ImVec2(1980.0F, 300.0F));
    ImGui::Begin("toolbar", nullptr, ImGuiWindowFlags_NoDecoration);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const char* name : k_toolbar_icons) {
        for (const bool active : {false, true}) {
            SCOPED_TRACE(std::string(name) + (active ? " active" : ""));
            const int first_vtx = dl->VtxBuffer.Size;
            ImGui::PushID(active ? 1 : 0);
            (void)ui::toggle_button(name, name, active, "tip");
            ImGui::PopID();
            ImGui::SameLine();
            const ImVec2 bmin = ImGui::GetItemRectMin();
            const ImVec2 bmax = ImGui::GetItemRectMax();
            EXPECT_FLOAT_EQ(bmax.x - bmin.x, bmax.y - bmin.y) << "square";
            // The glyph is the last quad drawn (after the optional fill).
            ASSERT_GE(dl->VtxBuffer.Size - first_vtx, 4);
            ImVec2 qmin(1e9F, 1e9F);
            ImVec2 qmax(-1e9F, -1e9F);
            for (int i = dl->VtxBuffer.Size - 4; i < dl->VtxBuffer.Size; ++i) {
                const ImVec2 p = dl->VtxBuffer[i].pos;
                qmin = ImVec2(std::min(qmin.x, p.x), std::min(qmin.y, p.y));
                qmax = ImVec2(std::max(qmax.x, p.x), std::max(qmax.y, p.y));
            }
            EXPECT_NEAR((qmin.x + qmax.x) * 0.5F, (bmin.x + bmax.x) * 0.5F, 0.5F);
            EXPECT_NEAR((qmin.y + qmax.y) * 0.5F, (bmin.y + bmax.y) * 0.5F, 0.5F);
            // And icon_origin agrees with what was drawn.
            ImVec2 ink_min;
            ImVec2 ink_max;
            (void)ui::icon_origin(ui::ic(name),
                                  ImVec2((bmin.x + bmax.x) * 0.5F, (bmin.y + bmax.y) * 0.5F),
                                  &ink_min, &ink_max);
            EXPECT_NEAR(ink_min.x, qmin.x, 1e-3F);
            EXPECT_NEAR(ink_max.y, qmax.y, 1e-3F);
        }
    }
    ImGui::End();
    ImGui::Render();
}

INSTANTIATE_TEST_SUITE_P(Scales, EditorIconCentring, ::testing::Values(1.0F, 1.25F, 1.5F, 2.0F));

} // namespace

#endif // ENGINE_EDITOR
