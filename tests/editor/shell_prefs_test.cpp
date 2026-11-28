// Editor preferences (ROADMAP E8.2): <project>/.quat/editor.ini is external
// data, so every line goes through a validating parser. These tests feed it
// hostile input (non-finite and overflowing numbers, paths leaving the
// project, thousands of lines) and check the round trip and the recent-scene
// list.

#ifdef ENGINE_EDITOR

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "editor/shell/prefs.hpp"
#include "engine/core/utf8_path.hpp"

namespace {

namespace sh = engine::editor::shell;

// Applies every line of `text`; returns how many were accepted.
int parse_all(const std::string& text, sh::EditorPrefs& p) {
    std::istringstream in(text);
    std::string line;
    int accepted = 0;
    while (std::getline(in, line)) accepted += sh::parse_pref_line(line, p) ? 1 : 0;
    return accepted;
}

TEST(EditorPrefs, WriteThenParseRoundTrips) {
    sh::EditorPrefs p = sh::default_prefs();
    p.light_theme = true;
    p.panel_open[static_cast<std::size_t>(sh::Panel::console)] = false;
    p.snap_enabled = true;
    p.snap_move = 0.25F;
    p.snap_rotate_deg = 45.0F;
    p.snap_scale = 0.5F;
    p.viewport_stats = false;
    p.scene_path = "scenes/level 1.json";
    p.recent_scenes = {"scenes/a.json", "scenes/b.json"};

    sh::EditorPrefs q = sh::default_prefs();
    EXPECT_GT(parse_all(sh::write_prefs(p), q), 0);
    EXPECT_TRUE(q.light_theme);
    EXPECT_EQ(q.panel_open, p.panel_open);
    EXPECT_TRUE(q.snap_enabled);
    EXPECT_FLOAT_EQ(q.snap_move, 0.25F);
    EXPECT_FLOAT_EQ(q.snap_rotate_deg, 45.0F);
    EXPECT_FLOAT_EQ(q.snap_scale, 0.5F);
    EXPECT_FALSE(q.viewport_stats);
    EXPECT_EQ(q.scene_path, p.scene_path);
    EXPECT_EQ(q.recent_scenes, p.recent_scenes);
}

TEST(EditorPrefs, NumbersMustBeFiniteAndAreClampedToTheUiRanges) {
    sh::EditorPrefs p = sh::default_prefs();
    const sh::EditorPrefs d = sh::default_prefs();
    for (const char* bad : {"SnapMove=inf", "SnapMove=-inf", "SnapMove=nan", "SnapMove=NaN",
                            "SnapMove=1e999", "SnapMove=-1e999", "SnapMove=", "SnapMove=1.5x",
                            "SnapMove= 2", "SnapMove=0x", "SnapRotate=infinity",
                            "SnapScale=1e40"}) {
        EXPECT_FALSE(sh::parse_pref_line(bad, p)) << bad;
    }
    EXPECT_FLOAT_EQ(p.snap_move, d.snap_move);
    EXPECT_FLOAT_EQ(p.snap_rotate_deg, d.snap_rotate_deg);
    EXPECT_FLOAT_EQ(p.snap_scale, d.snap_scale);

    EXPECT_TRUE(sh::parse_pref_line("SnapMove=1e9", p));
    EXPECT_FLOAT_EQ(p.snap_move, sh::SnapLimits::move_max);
    EXPECT_TRUE(sh::parse_pref_line("SnapMove=-3", p));
    EXPECT_FLOAT_EQ(p.snap_move, sh::SnapLimits::move_min);
    EXPECT_TRUE(sh::parse_pref_line("SnapRotate=720", p));
    EXPECT_FLOAT_EQ(p.snap_rotate_deg, sh::SnapLimits::rotate_max);
    EXPECT_TRUE(sh::parse_pref_line("SnapScale=0", p));
    EXPECT_FLOAT_EQ(p.snap_scale, sh::SnapLimits::scale_min);
}

TEST(EditorPrefs, MalformedFlagsPanelsAndThemesAreIgnored) {
    sh::EditorPrefs p = sh::default_prefs();
    for (const char* bad : {"Theme=purple", "Theme=", "SnapEnabled=2", "SnapEnabled=yes",
                            "ViewportStats=-1", "Panel=Console", "Panel=Console,7",
                            "Panel=Nope,0", "Panel=Viewport,0", "Unknown=1", "", "=", "Scene",
                            "[QuatEditor][Prefs]"}) {
        EXPECT_FALSE(sh::parse_pref_line(bad, p)) << bad;
    }
    EXPECT_FALSE(p.light_theme);
    for (const bool open : p.panel_open) EXPECT_TRUE(open) << "a non-closable panel stays open";
    EXPECT_TRUE(sh::parse_pref_line("Panel=Console,0\r", p)) << "CRLF files parse";
    EXPECT_FALSE(p.panel_open[static_cast<std::size_t>(sh::Panel::console)]);
}

TEST(EditorPrefs, ScenePathsMustStayInsideTheProject) {
    for (const char* bad : {"", "/etc/passwd.json", "../outside.json", "scenes/../../x.json",
                            "scenes/a.txt", "scenes/a", "//server/share/a.json"}) {
        EXPECT_NE(sh::scene_path_problem(bad), nullptr) << bad;
    }
    EXPECT_NE(sh::scene_path_problem(std::string(2000, 'a') + ".json"), nullptr);
    EXPECT_NE(sh::scene_path_problem(std::string("scenes/a\0b.json", 15)), nullptr);
    EXPECT_EQ(sh::scene_path_problem("scenes/a.json"), nullptr);
    EXPECT_EQ(sh::scene_path_problem("a.json"), nullptr);

    sh::EditorPrefs p = sh::default_prefs();
    for (const char* bad : {"Scene=../x.json", "Scene=/abs.json", "Scene=", "Recent=../x.json",
                            "Recent=/abs.json", "Recent=scenes/x.txt"}) {
        EXPECT_FALSE(sh::parse_pref_line(bad, p)) << bad;
    }
    EXPECT_EQ(p.scene_path, sh::default_prefs().scene_path);
    EXPECT_TRUE(p.recent_scenes.empty());
}

TEST(EditorPrefs, RecentScenesAreDeduplicatedAndCapped) {
    sh::EditorPrefs p = sh::default_prefs();
    std::string text;
    for (int i = 0; i < 1000; ++i) text += "Recent=scenes/s" + std::to_string(i % 20) + ".json\n";
    text += "Recent=../escape.json\n";
    (void)parse_all(text, p);
    ASSERT_EQ(p.recent_scenes.size(), sh::k_max_recent_scenes);
    for (std::size_t i = 0; i < p.recent_scenes.size(); ++i) {
        EXPECT_EQ(p.recent_scenes[i], "scenes/s" + std::to_string(i) + ".json") << "file order";
    }
    // Writing never exceeds the cap or emits a rejected path either.
    p.recent_scenes.push_back("/abs.json");
    p.recent_scenes.push_back("scenes/extra.json");
    sh::EditorPrefs q = sh::default_prefs();
    (void)parse_all(sh::write_prefs(p), q);
    EXPECT_EQ(q.recent_scenes.size(), sh::k_max_recent_scenes);
}

TEST(EditorPrefs, PushRecentMovesThePathToTheFront) {
    std::vector<std::string> r = {"a.json", "b.json", "c.json"};
    // Passing an element of the list itself (what Open Recent does).
    sh::push_recent(r, r[1]);
    EXPECT_EQ(r, (std::vector<std::string>{"b.json", "a.json", "c.json"}));
    sh::push_recent(r, "d.json");
    EXPECT_EQ(r, (std::vector<std::string>{"d.json", "b.json", "a.json", "c.json"}));
    sh::push_recent(r, "e.json", 3);
    EXPECT_EQ(r, (std::vector<std::string>{"e.json", "d.json", "b.json"}));
}

TEST(EditorPrefs, ScenePathsMustBeValidUtf8) {
    // Each would make path construction throw on Windows.
    for (const std::string& bad : {std::string("scenes/\xFF.json"),        // invalid byte
                                  std::string("scenes/\xC3.json"),        // truncated sequence
                                  std::string("scenes/\xC0\xAF.json"),    // overlong '/'
                                  std::string("scenes/\xE0\x80\xAF.json"), // overlong, 3 bytes
                                  std::string("scenes/\xED\xA0\x80.json"), // surrogate U+D800
                                  std::string("scenes/\xF4\x90\x80\x80.json"), // > U+10FFFF
                                  std::string("scenes/\x80.json")}) {    // stray continuation
        EXPECT_FALSE(engine::core::is_valid_utf8(bad)) << bad;
        EXPECT_NE(sh::scene_path_problem(bad), nullptr) << bad;
        sh::EditorPrefs p = sh::default_prefs();
        EXPECT_FALSE(sh::parse_pref_line("Scene=" + bad, p));
        EXPECT_FALSE(sh::parse_pref_line("Recent=" + bad, p));
    }
    for (const char* good : {"scenes/caf\xC3\xA9.json", "scenes/\xE6\x97\xA5\xE6\x9C\xAC.json",
                             "scenes/\xF0\x9F\x8C\xB2 trees.json", "scenes/\xEF\xBF\xBD.json"}) {
        EXPECT_TRUE(engine::core::is_valid_utf8(good)) << good;
        EXPECT_EQ(sh::scene_path_problem(good), nullptr) << good;
    }
}

TEST(EditorPrefs, AForcedThemeIsNotSaved) {
    // Saved dark, QUAT_EDITOR_THEME=light showing: the ini keeps dark.
    sh::ThemePreference t{.forced = true, .saved_light = false};
    EXPECT_FALSE(sh::theme_to_save(t, /*showing_light=*/true));
    t.saved_light = true;
    EXPECT_TRUE(sh::theme_to_save(t, false));
    // Once the user picks a theme the override is over: what shows is saved.
    t.forced = false;
    EXPECT_FALSE(sh::theme_to_save(t, false));
    EXPECT_TRUE(sh::theme_to_save(t, true));
}

} // namespace

#endif // ENGINE_EDITOR
