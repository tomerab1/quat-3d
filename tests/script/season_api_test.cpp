// R25.1: the Lua `season.get` / `season.set` bindings (the same surface as
// the remote season.* commands).

#include "engine/scene/season.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using namespace engine::scene;

TEST_F(ScriptTest, SeasonFromLua) {
    write("season.lua", R"(
        function on_start(self)
            print("before", season.get().in_scene)
            local s = season.set({ mode = "manual", season = "autumn", weather_seed = 9 })
            print("set", s.in_scene, s.mode, s.season, s.year_phase)
            print("weights", string.format("%.2f", s.weights.autumn))
            s = season.set({ mode = "independent", year_minutes = 40 })
            print("fast", string.format("%.1f", s.year_minutes))
            local ok, err = pcall(season.set, { season = "monsoon" })
            print("bad", ok, err)
            ok, err = pcall(season.set, { colour = 1 })
            print("unknown", ok, err)
        end
    )");
    (void)scripted("seasons", "season.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "before\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "set\ttrue\tmanual\tautumn\t0.625"));
    EXPECT_TRUE(has_line(log, "weights\t1.00"));
    EXPECT_TRUE(has_line(log, "fast\t40.0"));
    EXPECT_TRUE(has_line(log, "bad\tfalse\tseason.set: season: expected one of"));
    EXPECT_TRUE(has_line(log, "colour: unknown field"));
    const entt::entity e = find_season(scene.registry());
    ASSERT_NE(e, entt::entity{entt::null});
    EXPECT_EQ(scene.registry().get<SeasonState>(e).mode, SeasonMode::independent);
    EXPECT_EQ(scene.registry().get<SeasonState>(e).weather_seed, 9U);
}

} // namespace
} // namespace quat::test
