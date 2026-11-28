#pragma once

// Toast notifications (ROADMAP E8.1): short, auto-dismissing messages in
// the bottom-right corner (scene saved, load failed, values copied ...).
// A toast may carry one action button ("Show in folder" on a finished
// package).
// ToastQueue is plain logic (tested without ImGui); draw_toasts() renders
// the process-wide queue once per frame.

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

struct ImVec2;

namespace engine::editor::ui {

enum class ToastKind : std::uint8_t { info, success, warning, error };

// An optional button under a toast's text; running it dismisses the toast.
struct ToastAction {
    std::string           label; // empty: no button
    std::function<void()> run;
};

struct Toast {
    std::uint64_t id = 0;
    ToastKind     kind = ToastKind::info;
    std::string   text;
    float         lifetime = 4.0F; // seconds on screen
    float         age = 0.0F;
    ToastAction   action;
};

class ToastQueue {
public:
    static constexpr std::size_t k_max = 5;         // oldest dropped beyond this
    static constexpr float       k_fade_s = 0.35F;  // fade-out at the end of life

    // Adds a toast; repeats of the newest message restart its timer instead
    // of stacking duplicates (the repeat's action replaces the old one).
    // Returns its id.
    std::uint64_t push(ToastKind kind, std::string text, float lifetime = 4.0F,
                       ToastAction action = {});
    // Runs toast `id`'s action (if it has one) and dismisses it. False when
    // there is no such toast or it has no action.
    bool run_action(std::uint64_t id);
    // Ages every toast by `dt` (clamped to >= 0) and drops expired ones.
    void update(float dt);
    void dismiss(std::uint64_t id);
    void clear() { items_.clear(); }

    [[nodiscard]] const std::deque<Toast>& items() const { return items_; }
    // 1 while fresh, falling to 0 over the last k_fade_s seconds.
    [[nodiscard]] static float opacity(const Toast& t);

private:
    std::deque<Toast> items_;
    std::uint64_t     next_id_ = 1;
};

// The editor's queue.
[[nodiscard]] ToastQueue& toasts();
inline std::uint64_t notify(ToastKind kind, std::string text, float lifetime = 4.0F,
                            ToastAction action = {}) {
    return toasts().push(kind, std::move(text), lifetime, std::move(action));
}

// Ages the queue by `dt` and draws it stacked upwards from `bottom_right`
// (screen coordinates). Clicking a toast dismisses it; its action button
// runs the action.
void draw_toasts(float dt, const ImVec2& bottom_right);

} // namespace engine::editor::ui
