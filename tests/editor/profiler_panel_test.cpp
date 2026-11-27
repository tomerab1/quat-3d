// Editor Profiler panel (R4.2): draws in a headless ImGui context with and
// without a profiler, refreshes its rows each frame, and keeps the frozen
// snapshot when the profiler is reset underneath it.

#ifdef ENGINE_EDITOR

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstdint>
#include <vector>

#include "editor/profiler_panel.hpp"
#include "engine/core/frame_profiler.hpp"

namespace {

using engine::core::FrameProfiler;
using engine::core::ProfileCategory;

// A private ImGui context that manages its own textures (no renderer).
class HeadlessImGui {
public:
    HeadlessImGui() : previous_(ImGui::GetCurrentContext()), ctx_(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    ~HeadlessImGui() {
        ImGui::DestroyContext(ctx_);
        ImGui::SetCurrentContext(previous_);
    }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    template <typename Draw>
    void frame(Draw&& draw) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(900.0F, 500.0F));
        draw();
        ImGui::Render();
    }

private:
    ImGuiContext* previous_;
    ImGuiContext* ctx_;
};

TEST(ProfilerPanel, DrawsWithoutAProfiler) {
    HeadlessImGui imgui;
    engine::editor::ProfilerPanelState state;
    imgui.frame([&] { engine::editor::draw_profiler_panel(nullptr, state); });
    EXPECT_TRUE(state.zones.empty());
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
}

TEST(ProfilerPanel, ShowsZonesAndKeepsTheFrozenSnapshot) {
    HeadlessImGui imgui;
    FrameProfiler profiler(FrameProfiler::Config{.window = 16, .scope_capacity = 64});
    profiler.bind_current_thread();
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

    engine::editor::ProfilerPanelState state;
    for (int f = 0; f < 3; ++f) { // a few frames: tables size themselves first
        imgui.frame([&] { engine::editor::draw_profiler_panel(&profiler, state); });
    }
    ASSERT_EQ(state.zones.size(), 2U);
    EXPECT_EQ(state.frame_ms.size(), 3U); // the first frame has no wall time
    EXPECT_EQ(state.gpu_ms.size(), 4U);
    EXPECT_FLOAT_EQ(state.frame.gpu.avg, 5.0F);

    state.frozen = true;
    profiler.reset();
    imgui.frame([&] { engine::editor::draw_profiler_panel(&profiler, state); });
    EXPECT_EQ(state.zones.size(), 2U); // the snapshot, not the cleared profiler

    state.frozen = false;
    imgui.frame([&] { engine::editor::draw_profiler_panel(&profiler, state); });
    EXPECT_TRUE(state.zones.empty());

    state.open = false; // a closed panel draws and refreshes nothing
    imgui.frame([&] { engine::editor::draw_profiler_panel(&profiler, state); });
    profiler.unbind_current_thread();
}

} // namespace

#endif // ENGINE_EDITOR
