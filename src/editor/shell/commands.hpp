#pragma once

// Command registry (ROADMAP E8.2). Every editor action is registered once —
// id, label, icon, category, shortcut, enabled/checked predicates and the
// action — and the main menu, toolbar, command palette and keyboard
// shortcuts all go through it, so a shortcut shown in a menu is always the
// one that works and the palette lists everything the menus can do.
//
// Shortcuts are ImGui key chords (ImGuiMod_Ctrl | ImGuiKey_S). Their scope
// decides when they fire: global ones work everywhere except while typing
// into a text field (unless they carry Ctrl/Alt), viewport ones only while
// the viewport is hovered or focused, selection ones while the hierarchy or
// the viewport is.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace engine::editor::shell {

enum class ShortcutScope : std::uint8_t { global, viewport, selection };

struct Command {
    std::string           id{};       // "file.save"
    std::string           label{};    // "Save Scene"
    std::string           category{}; // "File" (palette grouping)
    const char*           icon = nullptr;
    ImGuiKeyChord         shortcut = 0; // 0 = none
    ShortcutScope         scope = ShortcutScope::global;
    std::string           hint{};     // tooltip / palette detail (one line)
    std::function<bool()> enabled{};  // null = always enabled
    std::function<bool()> checked{};  // null = not a toggle
    std::function<void()> action{};
};

// Where keyboard focus is, as the shortcut dispatcher needs it.
struct ShortcutContext {
    bool typing = false;          // a text field is active
    bool viewport = false;        // viewport hovered or focused
    bool selection_focus = false; // hierarchy or viewport focused / hovered
    bool camera_look = false;     // right-drag flying: letters belong to the camera
};

class CommandRegistry {
public:
    // Registers (or replaces) a command; returns it for further setup.
    Command& add(Command command);
    [[nodiscard]] const Command* find(std::string_view id) const;
    [[nodiscard]] std::span<const Command> all() const { return commands_; }

    [[nodiscard]] bool is_enabled(const Command& c) const { return !c.enabled || c.enabled(); }
    [[nodiscard]] bool is_checked(const Command& c) const { return c.checked && c.checked(); }

    // Runs `id` if it exists and is enabled; records it as recently used.
    bool run(std::string_view id);
    // Ids of recently run commands, newest first (palette ordering).
    [[nodiscard]] std::span<const std::string> recent() const { return recent_; }

    // Whether `c`'s shortcut may fire in `ctx` (scope + typing rules).
    [[nodiscard]] static bool shortcut_allowed(const Command& c, const ShortcutContext& ctx);
    // Fires the first enabled command whose chord was pressed this frame and
    // whose scope allows it. Returns the id that ran (empty if none). Needs a
    // current ImGui frame.
    std::string dispatch_shortcuts(const ShortcutContext& ctx);

private:
    std::vector<Command>     commands_;
    std::vector<std::string> recent_;
};

// "Ctrl+Shift+P", "F2", "Delete"; empty for 0. Uses ImGui's key names (no
// context needed).
[[nodiscard]] std::string format_chord(ImGuiKeyChord chord);

} // namespace engine::editor::shell
