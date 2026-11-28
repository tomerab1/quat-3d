// Editor Profiler panel (R4.2 on the E8 design system): the row filter, the
// refresh / freeze contract, drawing in a headless ImGui context with the
// editor fonts and theme (with and without a profiler, with no ImGui
// recoverable errors), and its registration as a closable shell panel.

#ifdef ENGINE_EDITOR

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/panels/profiler.hpp"
#include "editor/shell/panels.hpp"
#include "editor/ui/fonts.hpp"
#include "editor/ui/theme.hpp"
#include "engine/core/frame_profiler.hpp"

namespace {

namespace p = engine::editor::panels;
namespace ui = engine::editor::ui;
using engine::core::FrameProfiler;
using engine::core::ProfileCategory;

int         g_errors = 0;
std::string g_last_error;

// A private ImGui context with the editor fonts, managing its own textures
// (no renderer); counts ImGui's recoverable errors (unbalanced Push/Pop,
// Begin/End).
class HeadlessImGui {
public:
    HeadlessImGui() : previous_(ImGui::GetCurrentContext()), ctx_(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
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
        (void)ui::load_fonts(*io.Fonts, std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts",
                             ui::DpiScale{1.0F, 1.0F});
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() {
        ui::set_fonts({});
        ImGui::DestroyContext(ctx_);
        ImGui::SetCurrentContext(previous_);
    }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    // One frame with the mouse at `mouse`; returns the ID ImGui found
    // submitted twice under it (0: none).
    template <typename Draw>
    ImGuiID frame(Draw&& draw, ImVec2 mouse = ImVec2(-1.0F, -1.0F)) {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        const ImGuiID conflict = ctx_->DebugDrawIdConflictsId;
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F));
        draw();
        ImGui::Render();
        return conflict;
    }

private:
    ImGuiContext* previous_;
    ImGuiContext* ctx_;
};

// Four profiled frames with one pass (GPU timed) and one system (CPU).
void record_frames(FrameProfiler& profiler) {
    const auto pass = engine::core::intern_profile_name("panel_test.pass", ProfileCategory::pass);
    const auto system =
        engine::core::intern_profile_name("panel_test.system", ProfileCategory::system);
    for (int i = 0; i < 4; ++i) {
        {
            const engine::core::ProfileScope s(system);
        }
        const std::uint64_t index = profiler.frame_index();
        profiler.end_frame();
        const std::vector<engine::core::GpuZoneTiming> gpu{{pass, 1.0F + static_cast<float>(i)}};
        profiler.submit_gpu_frame(index, gpu, 5.0F);
    }
}

TEST(ProfilerPanel, RowFilterByKindAndText) {
    p::ProfilerState state;
    engine::core::ZoneSummary pass;
    pass.name = "gbuffer";
    pass.category = ProfileCategory::pass;
    engine::core::ZoneSummary system;
    system.name = "physics_system";
    system.category = ProfileCategory::system;

    EXPECT_TRUE(p::profiler_row_visible(state, pass));
    EXPECT_TRUE(p::profiler_row_visible(state, system));
    state.kind = p::ProfilerKind::pass;
    EXPECT_TRUE(p::profiler_row_visible(state, pass));
    EXPECT_FALSE(p::profiler_row_visible(state, system));
    state.kind = p::ProfilerKind::all;
    state.filter = "PHYS"; // case-insensitive substring
    EXPECT_FALSE(p::profiler_row_visible(state, pass));
    EXPECT_TRUE(p::profiler_row_visible(state, system));
    state.kind = p::ProfilerKind::wait;
    EXPECT_FALSE(p::profiler_row_visible(state, system)) << "both filters apply";
}

TEST(ProfilerPanel, RefreshFollowsTheProfilerUnlessFrozen) {
    FrameProfiler profiler(FrameProfiler::Config{.window = 16, .scope_capacity = 64});
    profiler.bind_current_thread();
    record_frames(profiler);

    p::ProfilerState state;
    p::refresh_profiler(profiler, state);
    ASSERT_EQ(state.zones.size(), 2U);
    EXPECT_EQ(state.frame_ms.size(), 3U); // the first frame has no wall time
    EXPECT_EQ(state.gpu_ms.size(), 4U);
    EXPECT_FLOAT_EQ(state.frame.gpu.avg, 5.0F);

    state.frozen = true;
    profiler.reset();
    p::refresh_profiler(profiler, state);
    EXPECT_EQ(state.zones.size(), 2U) << "the snapshot, not the cleared profiler";

    state.frozen = false;
    p::refresh_profiler(profiler, state);
    EXPECT_TRUE(state.zones.empty());
    profiler.unbind_current_thread();
}

TEST(ProfilerPanel, DrawsWithoutAProfiler) {
    HeadlessImGui imgui;
    p::ProfilerState state;
    bool open = true;
    (void)imgui.frame([&] { p::draw_profiler(nullptr, state, "Profiler", &open); });
    EXPECT_TRUE(state.zones.empty());
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0) << "the empty state is drawn";
    EXPECT_EQ(g_errors, 0) << g_last_error;
}

TEST(ProfilerPanel, DrawsZonesWithoutErrorsOrDuplicateIds) {
    HeadlessImGui imgui;
    FrameProfiler profiler(FrameProfiler::Config{.window = 16, .scope_capacity = 64});
    profiler.bind_current_thread();
    record_frames(profiler);

    p::ProfilerState state;
    bool open = true;
    const auto draw = [&] { p::draw_profiler(&profiler, state, "Profiler", &open); };
    for (int f = 0; f < 3; ++f) (void)imgui.frame(draw); // tables size themselves first
    ASSERT_EQ(state.zones.size(), 2U);
    // Hover every point of a grid: ImGui reports an ID submitted twice under
    // the mouse.
    for (float y = 4.0F; y < 600.0F; y += 12.0F) {
        for (float x = 4.0F; x < 900.0F; x += 24.0F) {
            ASSERT_EQ(imgui.frame(draw, ImVec2(x, y)), 0U) << "at " << x << ", " << y;
        }
    }
    EXPECT_EQ(g_errors, 0) << g_last_error;

    state.kind = p::ProfilerKind::wait; // no wait zones: the empty state
    (void)imgui.frame(draw);
    EXPECT_EQ(g_errors, 0) << g_last_error;

    open = false; // a closed panel draws and refreshes nothing
    profiler.reset();
    (void)imgui.frame(draw);
    EXPECT_EQ(state.zones.size(), 2U);
    profiler.unbind_current_thread();
}

TEST(ProfilerPanel, IsAClosableShellPanel) {
    namespace sh = engine::editor::shell;
    const sh::PanelInfo& info = sh::panel_info(sh::Panel::profiler);
    EXPECT_STREQ(info.title, "Profiler");
    EXPECT_TRUE(info.closable) << "listed as a View toggle";
    EXPECT_EQ(sh::find_panel("profiler"), sh::Panel::profiler);
    EXPECT_EQ(sh::panel_window_id(sh::Panel::profiler), "###Profiler");
}

} // namespace

#endif // ENGINE_EDITOR
