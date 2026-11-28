#include "commands.hpp"

#include <algorithm>

namespace engine::editor::shell {

namespace {
constexpr std::size_t k_max_recent = 8;
}

Command& CommandRegistry::add(Command command) {
    for (Command& c : commands_) {
        if (c.id == command.id) {
            c = std::move(command);
            return c;
        }
    }
    commands_.push_back(std::move(command));
    return commands_.back();
}

const Command* CommandRegistry::find(std::string_view id) const {
    for (const Command& c : commands_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

bool CommandRegistry::run(std::string_view id) {
    const Command* c = find(id);
    if (c == nullptr || !is_enabled(*c) || !c->action) return false;
    std::erase(recent_, std::string(id));
    recent_.insert(recent_.begin(), std::string(id));
    if (recent_.size() > k_max_recent) recent_.resize(k_max_recent);
    c->action();
    return true;
}

bool CommandRegistry::shortcut_allowed(const Command& c, const ShortcutContext& ctx) {
    if (c.shortcut == 0) return false;
    const bool has_ctrl_or_alt = (c.shortcut & (ImGuiMod_Ctrl | ImGuiMod_Alt)) != 0;
    const ImGuiKey key = static_cast<ImGuiKey>(c.shortcut & ~ImGuiMod_Mask_);
    const bool function_key = key >= ImGuiKey_F1 && key <= ImGuiKey_F24;
    // Plain letters / Delete belong to the text field while typing; chords
    // with Ctrl/Alt and function keys stay live (Ctrl+S while editing a name).
    if (ctx.typing && !has_ctrl_or_alt && !function_key) return false;
    // While flying the camera (right-drag), WASDQE belong to it.
    if (ctx.camera_look && !has_ctrl_or_alt && !function_key) return false;
    switch (c.scope) {
    case ShortcutScope::global: return true;
    case ShortcutScope::viewport: return ctx.viewport;
    case ShortcutScope::selection: return ctx.selection_focus;
    }
    return false;
}

std::string CommandRegistry::dispatch_shortcuts(const ShortcutContext& ctx) {
    for (const Command& c : commands_) {
        if (!shortcut_allowed(c, ctx)) continue;
        // No ImGui focus routing: our own scope rules decide.
        if (!ImGui::IsKeyChordPressed(c.shortcut)) continue;
        const std::string id = c.id; // run() may re-register commands
        if (run(id)) return id;
    }
    return {};
}

std::string format_chord(ImGuiKeyChord chord) {
    if (chord == 0) return {};
    std::string out;
    if ((chord & ImGuiMod_Ctrl) != 0) out += "Ctrl+";
    if ((chord & ImGuiMod_Shift) != 0) out += "Shift+";
    if ((chord & ImGuiMod_Alt) != 0) out += "Alt+";
    if ((chord & ImGuiMod_Super) != 0) out += "Super+";
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (key != ImGuiKey_None) out += ImGui::GetKeyName(key);
    return out;
}

} // namespace engine::editor::shell
