#pragma once

// input::Actions — evaluates an ActionMap against device input once per frame
// (G5.1). The C++ service behind Lua's `input.action(...)`, the remote
// `input.*` methods and CameraRigInput.
//
// Frame use: update(device, dt) once, after the device state is sampled.
// frame() is this frame's result (per-frame consumers: cameras, UI).
// Fixed-step consumers (the simulation, scripts' on_fixed_update) call
// take_sim_sample() once per step instead: it carries every started / ended
// edge since the previous take, so a press is seen by exactly one step even
// when a frame runs several steps or none.
//
// Contexts: a stack of the map's contexts, bottom to top; actions outside the
// stack are inactive. Evaluation runs top to bottom. A consuming context hides
// the controls its actuated actions read from the contexts below it; an
// exclusive context hides everything below it. Within one evaluation a key
// chord beats the plain key (ctrl+z suppresses z), more modifiers beating
// fewer. The device's keyboard_blocked / pointer_blocked flags gate keyboard
// and pointer controls (an action already actuated by the pointer keeps it).
//
// Determinism: the result depends only on the map, the context stack, the
// sequence of (device state, dt) and injections — no clocks, no randomness.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"
#include "engine/input/action_map.hpp"

namespace engine::input {

struct DeviceState;

struct ActionState {
    // Processed value: button in [0, 1] (x), axis1d (x), axis2d (x, y).
    glm::vec2 value{0.0F};
    bool  actuated = false; // a binding is past the press point (before the trigger)
    bool  active = false;   // the trigger's condition holds (input.action)
    bool  started = false;  // became active this frame / since the last take (action_pressed)
    bool  ended = false;    // stopped being active (action_released)
    float held_s = 0.0F;    // seconds actuated so far (0 on the press frame)
    // Drag triggers: the pointer at the press and now (viewport pixels).
    glm::vec2 drag_origin{0.0F};
    glm::vec2 pointer{0.0F};
    bool operator==(const ActionState&) const = default;
};

// One evaluation of every action, indexed like ActionMap::actions.
struct ActionSample {
    std::uint64_t            frame = 0; // updates so far
    std::vector<ActionState> states;
    bool operator==(const ActionSample&) const = default;
};

// Latches a stream of samples for a slower consumer: take() returns the
// latest sample with every started / ended edge since the previous take (a
// one-frame trigger that fired in between reads active). Actions uses one for
// take_sim_sample(); a consumer with its own fixed step nested inside the
// simulation step (scripts' on_fixed_update) keeps another.
class StepLatch {
public:
    void add(const ActionSample& sample);
    [[nodiscard]] ActionSample take(const ActionMap& map);

private:
    ActionSample      latest_;
    std::vector<char> started_;
    std::vector<char> ended_;
};

class Actions {
public:
    Actions();
    explicit Actions(ActionMap map);

    [[nodiscard]] const ActionMap& map() const { return map_; }
    // The map as loaded, before user rebinding.
    [[nodiscard]] const ActionMap& defaults() const { return defaults_; }
    // Replaces the map (and the defaults); the stack becomes the map's
    // active contexts, injections and runtime state reset, and rebinds of
    // actions the new map still has (same type) are kept.
    void set_map(ActionMap map);

    // ---- Contexts --------------------------------------------------------------
    // Pushes `name` on top (moving it there if already on the stack).
    [[nodiscard]] std::expected<void, core::Error> push_context(std::string_view name);
    // Removes `name` from the stack; false when it was not on it.
    bool pop_context(std::string_view name);
    // Context indices, bottom to top.
    [[nodiscard]] std::span<const int> context_stack() const { return stack_; }
    [[nodiscard]] bool context_active(std::string_view name) const;

    // Heading for camera_relative modifiers (radians, camera_framing.hpp
    // convention: forward = (cos yaw, 0, sin yaw)).
    void set_camera_yaw(float yaw_rad) { camera_yaw_ = yaw_rad; }

    // ---- Evaluation ------------------------------------------------------------
    void update(const DeviceState& device, float dt);
    [[nodiscard]] const ActionSample& frame() const { return frame_; }
    [[nodiscard]] ActionSample take_sim_sample();

    [[nodiscard]] int find(std::string_view name) const { return map_.find_action(name); }
    // This frame's state of `name`, or null when the map has no such action.
    [[nodiscard]] const ActionState* state(std::string_view name) const;

    // ---- Injection (tests, remote input.inject) -------------------------------
    // Makes `name` read `value` (after its bindings and modifiers, before the
    // trigger) for the next `frames` updates (1..36000).
    [[nodiscard]] std::expected<void, core::Error> inject(std::string_view name, glm::vec2 value,
                                                          int frames);
    void clear_injection(std::string_view name);
    void clear_injections() { injections_.clear(); }

    // ---- Rebinding (persisted per user) ---------------------------------------
    [[nodiscard]] std::expected<void, core::Error> rebind(std::string_view name,
                                                          std::vector<Binding> bindings);
    // Back to the map's own bindings: one action, or every action.
    [[nodiscard]] std::expected<void, core::Error> reset_bindings(std::string_view name);
    void reset_all_bindings();
    // {"version": 1, "bindings": {"<action>": [<binding>, ...]}} for every
    // action whose bindings differ from the defaults.
    [[nodiscard]] nlohmann::json overrides_json() const;
    // Applies an overrides document (validated whole before anything changes).
    [[nodiscard]] std::expected<void, core::Error> apply_overrides(const nlohmann::json& json);

private:
    struct Runtime {
        bool      was_actuated = false;
        bool      was_active = false;
        bool      pointer_captured = false; // actuated through a pointer control
        bool      hold_reached = false;
        bool      dragging = false;
        int       clicks = 0;
        float     since_press_s = 1.0e9F;
        glm::vec2 smoothed{0.0F};
    };
    struct Injection {
        int       action = -1;
        glm::vec2 value{0.0F};
        int       frames_left = 0;
    };

    void reset_runtime();
    void evaluate_trigger(std::size_t index, bool actuated, float dt, glm::vec2 pointer);

    ActionMap                defaults_;
    ActionMap                map_;
    std::vector<int>         stack_;
    std::vector<Runtime>     runtime_;
    std::vector<Injection>   injections_;
    ActionSample             frame_;
    StepLatch                sim_;
    float                    camera_yaw_ = 0.0F;
};

// The user's rebinding file: read (a missing file is not an error) and
// written atomically (temporary file + rename).
[[nodiscard]] std::expected<void, core::Error> load_overrides_file(Actions& actions,
                                                                   const std::filesystem::path& path);
[[nodiscard]] std::expected<void, core::Error> save_overrides_file(const Actions& actions,
                                                                   const std::filesystem::path& path);
// An action map asset read from disk (at most 1 MiB).
[[nodiscard]] std::expected<ActionMap, core::Error> load_action_map_file(const std::filesystem::path& path);

} // namespace engine::input
