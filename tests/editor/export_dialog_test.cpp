// File > Build & Export (ROADMAP E7) in the E8 shell: the "Show in folder"
// URL, toast actions, the completion toasts raised once per finished build,
// and the dialog drawn in a renderer-less ImGui context (no duplicate IDs,
// no recoverable ImGui errors, with and without an open game project).

#ifdef ENGINE_EDITOR

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "editor/shell/export_dialog.hpp"
#include "editor/ui/fonts.hpp"
#include "editor/ui/theme.hpp"
#include "editor/ui/toasts.hpp"
#include "engine/editor/editor.hpp"
#include "engine/project/build_job.hpp"
#include "engine/project/game_project.hpp"

namespace {

namespace ed = engine::editor;
namespace sh = engine::editor::shell;
namespace ui = engine::editor::ui;
namespace pj = engine::project;

TEST(EditorExportDialog, FileUrlPercentEncodes) {
    EXPECT_EQ(sh::file_url("/tmp/dist/game"), "file:///tmp/dist/game");
    EXPECT_EQ(sh::file_url("/tmp/a b/\xC3\xBC#"), "file:///tmp/a%20b/%C3%BC%23");
    EXPECT_EQ(sh::file_url("C:/Games/dist"), "file:///C:/Games/dist") << "drive paths get a third slash";
}

TEST(EditorUiToastActions, RunOnceThenDismiss) {
    ui::ToastQueue q;
    int runs = 0;
    const auto plain = q.push(ui::ToastKind::info, "plain");
    const auto acted = q.push(ui::ToastKind::success, "Packaged", 4.0F,
                              {.label = "Show in folder", .run = [&] { ++runs; }});
    EXPECT_FALSE(q.run_action(plain)) << "no action";
    EXPECT_FALSE(q.run_action(9999)) << "no such toast";
    EXPECT_TRUE(q.run_action(acted));
    EXPECT_EQ(runs, 1);
    ASSERT_EQ(q.items().size(), 1U) << "running the action dismisses the toast";
    EXPECT_EQ(q.items().front().id, plain);
    EXPECT_FALSE(q.run_action(acted));
    EXPECT_EQ(runs, 1);
}

TEST(EditorUiToastActions, RepeatTakesTheNewAction) {
    ui::ToastQueue q;
    int first = 0;
    int second = 0;
    const auto a = q.push(ui::ToastKind::success, "Packaged", 4.0F,
                          {.label = "Show", .run = [&] { ++first; }});
    const auto b = q.push(ui::ToastKind::success, "Packaged", 4.0F,
                          {.label = "Show", .run = [&] { ++second; }});
    ASSERT_EQ(a, b);
    EXPECT_TRUE(q.run_action(a));
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 1);
}

class EditorExportToasts : public ::testing::Test {
protected:
    void SetUp() override { ui::toasts().clear(); }
    void TearDown() override { ui::toasts().clear(); }
};

TEST_F(EditorExportToasts, OncePerFinishedBuild) {
    sh::ExportDialogState state;
    pj::BuildStatus status;
    status.state = pj::BuildState::running;
    status.job = 1;
    sh::notify_build_finished(state, status);
    EXPECT_TRUE(ui::toasts().items().empty()) << "running: no toast";

    status.state = pj::BuildState::succeeded;
    status.package = "/tmp/dist/coin-collector-1.0.0-linux-x64.tar.gz";
    status.folder = "/tmp/dist/coin-collector-1.0.0-linux-x64";
    sh::notify_build_finished(state, status);
    ASSERT_EQ(ui::toasts().items().size(), 1U);
    const ui::Toast& done = ui::toasts().items().back();
    EXPECT_EQ(done.kind, ui::ToastKind::success);
    EXPECT_EQ(done.text, "Packaged coin-collector-1.0.0-linux-x64.tar.gz");
    EXPECT_EQ(done.action.label, "Show in folder");

    sh::notify_build_finished(state, status);
    EXPECT_EQ(ui::toasts().items().size(), 1U) << "the same job is announced once";

    status.job = 2;
    status.state = pj::BuildState::cancelled;
    sh::notify_build_finished(state, status);
    EXPECT_EQ(ui::toasts().items().size(), 1U) << "cancelling is the user's own action";

    status.job = 3;
    status.state = pj::BuildState::failed;
    status.error = "quat-pack exited with status 1";
    sh::notify_build_finished(state, status);
    ASSERT_EQ(ui::toasts().items().size(), 2U);
    const ui::Toast failed = ui::toasts().items().back();
    EXPECT_EQ(failed.kind, ui::ToastKind::error);
    EXPECT_EQ(failed.text, "Build & Export failed: quat-pack exited with status 1");
    EXPECT_EQ(failed.action.label, "Show log");
    EXPECT_FALSE(state.visible);
    EXPECT_TRUE(ui::toasts().run_action(failed.id));
    EXPECT_TRUE(state.visible) << "Show log reopens the dialog";
}

TEST_F(EditorExportToasts, SucceededWithoutFolderHasNoAction) {
    sh::ExportDialogState state;
    pj::BuildStatus status;
    status.state = pj::BuildState::succeeded;
    status.job = 1;
    status.package = "/tmp/dist/x.zip";
    sh::notify_build_finished(state, status);
    ASSERT_EQ(ui::toasts().items().size(), 1U);
    EXPECT_TRUE(ui::toasts().items().back().action.label.empty());
}

int         g_errors = 0;
std::string g_last_error;

class EditorExportDialogDraw : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 900.0F);
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

        editor_.build_job = &job_;
        editor_.start_export = [this](const ed::ExportRequest& r)
            -> std::expected<std::uint64_t, engine::core::Error> {
            requests_.push_back(r);
            return std::unexpected(engine::core::Error{"not in a test"});
        };
    }
    void TearDown() override {
        ui::toasts().clear();
        ui::set_fonts({});
        ImGui::DestroyContext(ctx_);
    }

    ImGuiID frame(ImVec2 mouse) {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        const ImGuiID conflict = ctx_->DebugDrawIdConflictsId;
        sh::draw_export_dialog(state_, editor_);
        ImGui::Render();
        return conflict;
    }

    void sweep(const char* what) {
        ui::apply_theme(ui::dark_theme(), 1.0F);
        for (int i = 0; i < 3; ++i) (void)frame(ImVec2(-1.0F, -1.0F));
        for (float y = 2.0F; y < 900.0F; y += 11.0F) {
            for (float x = 4.0F; x < 1280.0F; x += 37.0F) {
                ASSERT_EQ(frame(ImVec2(x, y)), 0U)
                    << what << ": duplicate ImGui ID under (" << x << ", " << y << ")";
            }
        }
        EXPECT_EQ(g_errors, 0) << what << ": " << g_last_error;
    }

    ImGuiContext*                  ctx_ = nullptr;
    pj::BuildJob                   job_;
    ed::EditorContext              editor_;
    sh::ExportDialogState          state_;
    std::vector<ed::ExportRequest> requests_;
};

TEST_F(EditorExportDialogDraw, NeedsABuildJob) {
    editor_.build_job = nullptr;
    state_.open_request = true;
    (void)frame(ImVec2(-1.0F, -1.0F));
    EXPECT_FALSE(state_.visible);
    EXPECT_TRUE(state_.open_request) << "kept until the dialog can open";
}

TEST_F(EditorExportDialogDraw, WithoutAGameProject) {
    state_.open_request = true;
    (void)frame(ImVec2(-1.0F, -1.0F));
    EXPECT_TRUE(state_.visible);
    EXPECT_FALSE(state_.open_request);
    EXPECT_EQ(state_.project, "games/coin-collector") << "seeded with the sample";
    EXPECT_EQ(state_.output, "dist");
    sweep("no game project");
    EXPECT_TRUE(requests_.empty()) << "hovering never starts a build";
}

TEST_F(EditorExportDialogDraw, WithAnOpenGameProject) {
    pj::GameProject game;
    game.name = "Coin Collector";
    game.id = "dev.quat3d.coin-collector";
    game.version = "1.0.0";
    editor_.game = &game;
    editor_.game_root = "/projects/coin-collector";
    state_.open_request = true;
    (void)frame(ImVec2(-1.0F, -1.0F));
    EXPECT_EQ(state_.project, "/projects/coin-collector");
    sweep("open game project");
    EXPECT_TRUE(requests_.empty());
}

} // namespace

#endif // ENGINE_EDITOR
