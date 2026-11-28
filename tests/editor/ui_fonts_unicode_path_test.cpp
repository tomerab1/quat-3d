// The editor's UI fonts load from a data root whose path is not ASCII.
//
// Release packages are smoke-tested unpacked under "smoke dir ü 画". The
// packaged editor (`game`) reads its fonts from <data root>/assets/fonts; on
// Windows the path went to ImGui through path::string(), which throws for
// characters outside the ANSI code page, and the exception-free build died
// during startup (exit 127 under Git Bash). Fonts load through UTF-8 paths
// (ImGui opens them with the wide API on Windows); this test copies them under
// such a directory and requires every face to load, on every platform.

#ifdef ENGINE_EDITOR

#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <imgui.h>

#include "editor/ui/fonts.hpp"
#include "engine/core/utf8_path.hpp"

namespace {

namespace fs = std::filesystem;
namespace ui = engine::editor::ui;

class EditorFontsUnicodePath : public ::testing::Test {
protected:
    void SetUp() override {
        const fs::path source =
            engine::core::path_from_utf8(QUAT_PROJECT_ROOT) / "assets" / "fonts";
        const std::string test =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        root_ = fs::temp_directory_path() /
                engine::core::path_from_utf8("quat fonts \xC3\xBC \xE7\x94\xBB " + test);
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
        ASSERT_FALSE(ec) << ec.message();
        fs::copy(source, root_ / "fonts", fs::copy_options::recursive, ec);
        ASSERT_FALSE(ec) << ec.message();
        ctx_ = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
    }
    void TearDown() override {
        ui::set_fonts({});
        if (ctx_ != nullptr) ImGui::DestroyContext(ctx_);
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path      root_;
    ImGuiContext* ctx_ = nullptr;
};

TEST_F(EditorFontsUnicodePath, EveryFaceLoads) {
    ImGuiIO& io = ImGui::GetIO();
    const ui::FontSet set = ui::load_fonts(*io.Fonts, root_ / "fonts", ui::DpiScale{1.0F, 1.0F});
    EXPECT_TRUE(set.inter) << "Inter not loaded from " << engine::core::path_to_utf8(root_);
    EXPECT_TRUE(set.mono);
    EXPECT_TRUE(set.icons);
    unsigned char* pixels = nullptr;
    int w = 0;
    int h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    EXPECT_NE(pixels, nullptr);
    EXPECT_GT(w, 0);
}

TEST_F(EditorFontsUnicodePath, MissingFontsFallBackToTheBuiltInFont) {
    ImGuiIO& io = ImGui::GetIO();
    const ui::FontSet set = ui::load_fonts(*io.Fonts, root_ / "absent", ui::DpiScale{1.0F, 1.0F});
    EXPECT_FALSE(set.inter);
    EXPECT_NE(set.fonts[0], nullptr);
}

} // namespace

#endif // ENGINE_EDITOR
