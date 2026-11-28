// Editor shell (ROADMAP E8.2): the command registry the menus, toolbar,
// palette and shortcuts share; shortcut scoping; chord names; the fuzzy
// matcher behind the command palette; the panel table.

#ifdef ENGINE_EDITOR

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>

#include "editor/shell/commands.hpp"
#include "editor/shell/fuzzy.hpp"
#include "editor/shell/panels.hpp"

namespace {

namespace sh = engine::editor::shell;

TEST(EditorShellCommands, RegistryRunsEnabledCommandsAndTracksRecent) {
    sh::CommandRegistry reg;
    int saves = 0;
    bool can_undo = false;
    reg.add({.id = "file.save", .label = "Save", .category = "File",
             .action = [&] { ++saves; }});
    reg.add({.id = "edit.undo", .label = "Undo", .category = "Edit",
             .enabled = [&] { return can_undo; }, .action = [] {}});
    ASSERT_NE(reg.find("file.save"), nullptr);
    EXPECT_EQ(reg.find("missing"), nullptr);

    EXPECT_TRUE(reg.run("file.save"));
    EXPECT_EQ(saves, 1);
    EXPECT_FALSE(reg.run("edit.undo")) << "disabled commands do not run";
    EXPECT_FALSE(reg.run("missing"));
    can_undo = true;
    EXPECT_TRUE(reg.run("edit.undo"));
    ASSERT_EQ(reg.recent().size(), 2U);
    EXPECT_EQ(reg.recent()[0], "edit.undo") << "newest first";
    EXPECT_TRUE(reg.run("file.save"));
    EXPECT_EQ(reg.recent()[0], "file.save");
    EXPECT_EQ(reg.recent().size(), 2U) << "re-running moves, not duplicates";

    // Re-registering an id replaces it in place (no duplicate menu entries).
    reg.add({.id = "file.save", .label = "Save Scene", .category = "File", .action = [] {}});
    EXPECT_EQ(reg.all().size(), 2U);
    EXPECT_EQ(reg.find("file.save")->label, "Save Scene");
}

TEST(EditorShellCommands, CheckedStateComesFromThePredicate) {
    sh::CommandRegistry reg;
    bool snap = false;
    const sh::Command& c = reg.add({.id = "gizmo.snap", .label = "Snap",
                                    .checked = [&] { return snap; },
                                    .action = [&] { snap = !snap; }});
    EXPECT_FALSE(reg.is_checked(c));
    EXPECT_TRUE(reg.run("gizmo.snap"));
    EXPECT_TRUE(reg.is_checked(*reg.find("gizmo.snap")));
}

TEST(EditorShellCommands, ShortcutScopes) {
    const sh::Command save{.id = "s", .shortcut = ImGuiMod_Ctrl | ImGuiKey_S};
    const sh::Command move{.id = "w", .shortcut = ImGuiKey_W, .scope = sh::ShortcutScope::viewport};
    const sh::Command del{.id = "d", .shortcut = ImGuiKey_Delete,
                          .scope = sh::ShortcutScope::selection};
    const sh::Command play{.id = "p", .shortcut = ImGuiKey_F5};
    const sh::Command none{.id = "n"};

    sh::ShortcutContext idle;
    EXPECT_TRUE(sh::CommandRegistry::shortcut_allowed(save, idle));
    EXPECT_FALSE(sh::CommandRegistry::shortcut_allowed(move, idle)) << "viewport only";
    EXPECT_FALSE(sh::CommandRegistry::shortcut_allowed(del, idle)) << "needs selection focus";
    EXPECT_FALSE(sh::CommandRegistry::shortcut_allowed(none, idle)) << "no chord, never";

    sh::ShortcutContext viewport{.viewport = true, .selection_focus = true};
    EXPECT_TRUE(sh::CommandRegistry::shortcut_allowed(move, viewport));
    EXPECT_TRUE(sh::CommandRegistry::shortcut_allowed(del, viewport));

    // Typing: letters and Delete belong to the text field; Ctrl chords and
    // function keys still work.
    sh::ShortcutContext typing{.typing = true, .viewport = true, .selection_focus = true};
    EXPECT_FALSE(sh::CommandRegistry::shortcut_allowed(move, typing));
    EXPECT_FALSE(sh::CommandRegistry::shortcut_allowed(del, typing));
    EXPECT_TRUE(sh::CommandRegistry::shortcut_allowed(save, typing));
    EXPECT_TRUE(sh::CommandRegistry::shortcut_allowed(play, typing));

    // Flying the camera (right-drag): WASD/QE move it, tools stay put.
    sh::ShortcutContext flying{.viewport = true, .camera_look = true};
    EXPECT_FALSE(sh::CommandRegistry::shortcut_allowed(move, flying));
    EXPECT_TRUE(sh::CommandRegistry::shortcut_allowed(save, flying));
}

TEST(EditorShellCommands, ChordNames) {
    EXPECT_EQ(sh::format_chord(0), "");
    EXPECT_EQ(sh::format_chord(ImGuiMod_Ctrl | ImGuiKey_S), "Ctrl+S");
    EXPECT_EQ(sh::format_chord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P), "Ctrl+Shift+P");
    EXPECT_EQ(sh::format_chord(ImGuiKey_F2), "F2");
    EXPECT_EQ(sh::format_chord(ImGuiMod_Shift | ImGuiKey_F5), "Shift+F5");
    EXPECT_EQ(sh::format_chord(ImGuiKey_Delete), "Delete");
}

TEST(EditorShellFuzzy, SubsequenceRequiredCaseInsensitive) {
    EXPECT_TRUE(sh::fuzzy_score("ss", "Save Scene").has_value());
    EXPECT_TRUE(sh::fuzzy_score("SAVE", "save scene").has_value());
    EXPECT_FALSE(sh::fuzzy_score("xyz", "Save Scene").has_value());
    EXPECT_FALSE(sh::fuzzy_score("sceness", "Save Scene").has_value()) << "order matters";
    EXPECT_EQ(sh::fuzzy_score("", "anything"), 0);
    EXPECT_FALSE(sh::fuzzy_score("a", "").has_value());
}

TEST(EditorShellFuzzy, RanksWordStartsAndPrefixesFirst) {
    const std::vector<std::string_view> texts = {
        "Toggle Snapping Tools",   // s..s inside words
        "Save Scene File",         // both at word starts, at the front
        "Reset Layout View",       // no match for "ss"
    };
    const std::vector<std::size_t> order = sh::fuzzy_rank("ss", texts);
    ASSERT_EQ(order.size(), 2U);
    EXPECT_EQ(order[0], 1U);
    EXPECT_EQ(order[1], 0U);

    // Consecutive runs beat scattered letters.
    EXPECT_GT(*sh::fuzzy_score("scene", "Save Scene"), *sh::fuzzy_score("scene", "Show Console Entries"));
    // Exact prefix beats a later word.
    EXPECT_GT(*sh::fuzzy_score("play", "Play"), *sh::fuzzy_score("play", "Display Stats"));
}

TEST(EditorShellFuzzy, EmptyQueryKeepsOrder) {
    const std::vector<std::string_view> texts = {"b", "a", "c"};
    EXPECT_EQ(sh::fuzzy_rank("", texts), (std::vector<std::size_t>{0, 1, 2}));
}

TEST(EditorShellPanels, NamesKeepStableIds) {
    for (const sh::PanelInfo& p : sh::panel_infos()) {
        const std::string name = sh::panel_window_name(p.id);
        const std::string id = sh::panel_window_id(p.id);
        EXPECT_EQ(id, std::string("###") + p.title);
        ASSERT_GE(name.size(), id.size());
        EXPECT_EQ(name.substr(name.size() - id.size()), id) << "the ID ends the window name";
        EXPECT_NE(name.find(p.title), std::string::npos);
        EXPECT_EQ(sh::find_panel(p.title), p.id);
    }
    EXPECT_EQ(sh::find_panel("assets"), sh::Panel::assets) << "case-insensitive";
    EXPECT_FALSE(sh::find_panel("Nope").has_value());
    EXPECT_FALSE(sh::panel_info(sh::Panel::viewport).closable) << "the viewport always exists";
}

} // namespace

#endif // ENGINE_EDITOR
