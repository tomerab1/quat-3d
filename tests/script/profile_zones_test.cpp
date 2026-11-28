// R4: the script runtime's work shows up in the frame profiler (and Tracy)
// as named zones — the tick (script_update, with its fixed and variable
// callback passes), trigger dispatch, the session start and the first load
// of a module — so a slow script is attributable from profile.frame and the
// editor's Profiler panel.

#include <algorithm>
#include <string_view>
#include <vector>

#include "engine/core/frame_profiler.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using engine::core::FrameProfiler;

[[nodiscard]] const engine::core::ZoneSummary* find_zone(
    const std::vector<engine::core::ZoneSummary>& zones, std::string_view name) {
    const auto it = std::ranges::find(zones, name, &engine::core::ZoneSummary::name);
    return it != zones.end() ? &*it : nullptr;
}

TEST_F(ScriptTest, TickIsProfiledAsNamedZones) {
    write("spin.lua", "function on_update(self, dt) self.t = (self.t or 0) + dt end\n"
                      "function on_fixed_update(self, dt) end");
    scripted("spinner", "spin.lua");

    FrameProfiler profiler(FrameProfiler::Config{.window = 8, .scope_capacity = 256});
    // Unbinds on every exit (a failed ASSERT included): later tests on this
    // thread must not record into the destroyed profiler.
    struct Binding {
        FrameProfiler& p;
        explicit Binding(FrameProfiler& target) : p(target) { p.bind_current_thread(); }
        ~Binding() { p.unbind_current_thread(); }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
    } binding(profiler);
    start();
    frames(1); // the first tick loads spin.lua
    profiler.end_frame();

    const std::vector<engine::core::ZoneSummary> zones = profiler.zones();
    for (const std::string_view name : {"script_start", "script_update", "script_on_fixed_update",
                                        "script_on_update", "script_load_module"}) {
        const engine::core::ZoneSummary* z = find_zone(zones, name);
        ASSERT_NE(z, nullptr) << name;
        EXPECT_EQ(z->calls, 1U) << name;
        EXPECT_EQ(z->cpu.count, 1U) << name;
    }

    // A paused clock (dt 0) skips the tick entirely, so no zone is opened.
    profiler.reset();
    runtime->update(0.0F);
    profiler.end_frame();
    EXPECT_EQ(find_zone(profiler.zones(), "script_update"), nullptr);
}

} // namespace
} // namespace quat::test
