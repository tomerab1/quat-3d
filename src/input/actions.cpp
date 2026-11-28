// input::Actions — see include/engine/input/actions.hpp.

#include "engine/input/actions.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/input/device_state.hpp"
#include "input_internal.hpp"

namespace engine::input {

namespace {

constexpr float k_axis_epsilon = 1.0e-3F;
constexpr int   k_max_inject_frames = 36000;

[[nodiscard]] std::size_t idx(int i) { return static_cast<std::size_t>(i); }

[[nodiscard]] glm::vec2 project(ActionType type, glm::vec2 v) {
    switch (type) {
    case ActionType::button: return glm::vec2(std::clamp(glm::length(v), 0.0F, 1.0F), 0.0F);
    case ActionType::axis1d: return glm::vec2(v.x, 0.0F);
    case ActionType::axis2d: return v;
    }
    return v;
}

[[nodiscard]] bool one_frame(TriggerKind k) {
    return k == TriggerKind::pressed || k == TriggerKind::released || k == TriggerKind::tap ||
           k == TriggerKind::double_click;
}

} // namespace

Actions::Actions() : Actions(default_action_map()) {}

Actions::Actions(ActionMap map) { set_map(std::move(map)); }

void Actions::set_map(ActionMap map) {
    // Keep the user's rebinds for actions the new map still has.
    std::vector<std::pair<std::string, std::vector<Binding>>> kept;
    for (const ActionDesc& a : map_.actions) {
        const int d = defaults_.find_action(a.name);
        if (d >= 0 && defaults_.actions[idx(d)].bindings != a.bindings) kept.emplace_back(a.name, a.bindings);
    }
    defaults_ = std::move(map);
    map_ = defaults_;
    for (auto& [name, bindings] : kept) {
        const int i = map_.find_action(name);
        if (i >= 0 && map_.actions[idx(i)].type == defaults_.actions[idx(i)].type) {
            map_.actions[idx(i)].bindings = std::move(bindings);
        }
    }
    stack_.clear();
    for (std::size_t i = 0; i < map_.contexts.size(); ++i) {
        if (map_.contexts[i].active) stack_.push_back(static_cast<int>(i));
    }
    injections_.clear();
    reset_runtime();
}

void Actions::reset_runtime() {
    runtime_.assign(map_.actions.size(), Runtime{});
    frame_.states.assign(map_.actions.size(), ActionState{});
    sim_ = StepLatch{};
}

std::expected<void, core::Error> Actions::push_context(std::string_view name) {
    const int c = map_.find_context(name);
    if (c < 0) {
        std::string known;
        for (const ContextDesc& d : map_.contexts) known += (known.empty() ? "" : ", ") + d.name;
        return std::unexpected(core::Error{"unknown input context '" + std::string(name) + "' (known: " + known + ")"});
    }
    std::erase(stack_, c);
    stack_.push_back(c);
    return {};
}

bool Actions::pop_context(std::string_view name) {
    const int c = map_.find_context(name);
    return c >= 0 && std::erase(stack_, c) > 0;
}

bool Actions::context_active(std::string_view name) const {
    const int c = map_.find_context(name);
    return c >= 0 && std::find(stack_.begin(), stack_.end(), c) != stack_.end();
}

const ActionState* Actions::state(std::string_view name) const {
    const int i = find(name);
    return i < 0 ? nullptr : &frame_.states[idx(i)];
}

void Actions::evaluate_trigger(std::size_t i, bool actuated, float dt, glm::vec2 pointer) {
    const ActionDesc& a = map_.actions[i];
    Runtime& rt = runtime_[i];
    ActionState& st = frame_.states[i];
    const bool pressed = actuated && !rt.was_actuated;
    const bool released = !actuated && rt.was_actuated;
    st.held_s = actuated ? (rt.was_actuated ? st.held_s + dt : 0.0F) : 0.0F;
    st.pointer = pointer;
    if (pressed) st.drag_origin = pointer;
    rt.since_press_s += dt;

    bool active = false;
    switch (a.trigger.kind) {
    case TriggerKind::down: active = actuated; break;
    case TriggerKind::pressed: active = pressed; break;
    case TriggerKind::released: active = released; break;
    case TriggerKind::tap: active = released && rt.since_press_s <= a.trigger.param; break;
    case TriggerKind::hold: active = actuated && st.held_s >= a.trigger.param; break;
    case TriggerKind::double_click:
        if (pressed) {
            if (rt.clicks == 1 && rt.since_press_s <= a.trigger.param) {
                active = true;
                rt.clicks = 0;
            } else {
                rt.clicks = 1;
            }
        }
        break;
    case TriggerKind::drag:
        // A click is not a drag: the pointer must leave the dead zone first.
        if (!actuated) {
            rt.dragging = false;
        } else if (!rt.dragging && glm::length(pointer - st.drag_origin) > a.trigger.param) {
            rt.dragging = true;
        }
        active = rt.dragging;
        break;
    }
    if (pressed) rt.since_press_s = 0.0F;
    st.actuated = actuated;
    st.active = active;
    st.started = active && !rt.was_active;
    st.ended = !active && rt.was_active;
    rt.was_actuated = actuated;
    rt.was_active = active;
}

void Actions::update(const DeviceState& device, float dt) {
    if (!(dt >= 0.0F) || !std::isfinite(dt)) dt = 0.0F;
    ++frame_.frame;

    // Actions of the contexts on the stack, top first.
    std::vector<const ActionDesc*> live;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        for (std::size_t i = 0; i < map_.actions.size(); ++i) {
            if (map_.actions[i].context == map_.contexts[idx(*it)].name) {
                live.push_back(&map_.actions[i]);
            }
        }
        if (map_.contexts[idx(*it)].exclusive) break;
    }
    std::array<std::uint8_t, DeviceState::key_count> chord_best{};
    chord_priorities(device, live, chord_best);

    const glm::vec2 pointer = device.pointer();
    Consumed consumed;
    std::vector<char> evaluated(map_.actions.size(), 0);
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        const ContextDesc& ctx_desc = map_.contexts[idx(*it)];
        Consumed added; // this context's reads hide controls from the ones below only
        for (std::size_t i = 0; i < map_.actions.size(); ++i) {
            const ActionDesc& a = map_.actions[i];
            if (a.context != ctx_desc.name || evaluated[i] != 0) continue;
            evaluated[i] = 1;
            Runtime& rt = runtime_[i];
            ReadContext rc{&device, &consumed, &chord_best,
                           !device.pointer_blocked || rt.pointer_captured};
            glm::vec2 best(0.0F);
            bool pointer_used = false;
            for (const Binding& b : a.bindings) {
                bool uses_pointer = false;
                const glm::vec2 v = read_binding(b, rc, uses_pointer);
                // The strongest binding wins (a stick and keys together do
                // not add up past full deflection).
                if (glm::length(v) > glm::length(best)) {
                    best = v;
                    pointer_used = uses_pointer;
                }
            }
            glm::vec2 value = project(a.type, best);
            for (const Modifier& m : a.modifiers) {
                if (m.kind == ModifierKind::camera_relative) {
                    // (right, forward) -> world (x, z) for the camera heading.
                    const glm::vec2 fwd(std::cos(camera_yaw_), std::sin(camera_yaw_));
                    const glm::vec2 right(-fwd.y, fwd.x);
                    value = right * value.x + fwd * value.y;
                } else if (m.kind != ModifierKind::smoothing) {
                    apply_modifier(m, value);
                }
            }
            for (const Injection& inj : injections_) {
                if (inj.action == static_cast<int>(i)) value = project(a.type, inj.value);
            }
            const float magnitude = glm::length(value);
            const bool actuated = a.type == ActionType::button ? magnitude >= a.press_point
                                                               : magnitude > k_axis_epsilon;
            // Smoothing changes the reported value, not the actuation.
            for (const Modifier& m : a.modifiers) {
                if (m.kind != ModifierKind::smoothing) continue;
                const float h = m.value.x;
                const float k = h > 0.0F ? 1.0F - std::exp2(-dt / h) : 1.0F;
                rt.smoothed += (value - rt.smoothed) * k;
                value = rt.smoothed;
            }
            rt.pointer_captured = actuated && (pointer_used || (rt.pointer_captured && rt.was_actuated));
            frame_.states[i].value = value;
            evaluate_trigger(i, actuated, dt, pointer);
            if (ctx_desc.consume && actuated) {
                for (const Binding& b : a.bindings) {
                    bool ignored = false;
                    if (read_binding(b, rc, ignored) != glm::vec2(0.0F)) consume_binding(b, added);
                }
            }
        }
        consumed.keys |= added.keys;
        consumed.mouse |= added.mouse;
        consumed.pad_buttons |= added.pad_buttons;
        consumed.pad_axes |= added.pad_axes;
        consumed.delta = consumed.delta || added.delta;
        consumed.wheel = consumed.wheel || added.wheel;
        consumed.pointer = consumed.pointer || added.pointer;
        if (ctx_desc.exclusive) break;
    }
    // Actions outside the stack (or below an exclusive context) go inactive.
    for (std::size_t i = 0; i < map_.actions.size(); ++i) {
        if (evaluated[i] != 0) continue;
        frame_.states[i].value = glm::vec2(0.0F);
        runtime_[i].smoothed = glm::vec2(0.0F);
        runtime_[i].pointer_captured = false;
        evaluate_trigger(i, false, dt, pointer);
    }

    for (Injection& inj : injections_) --inj.frames_left;
    std::erase_if(injections_, [](const Injection& inj) { return inj.frames_left <= 0; });

    sim_.add(frame_);
}

ActionSample Actions::take_sim_sample() {
    if (frame_.frame == 0) return frame_; // nothing evaluated yet
    return sim_.take(map_);
}

void StepLatch::add(const ActionSample& sample) {
    if (started_.size() != sample.states.size()) {
        started_.assign(sample.states.size(), 0);
        ended_.assign(sample.states.size(), 0);
    }
    for (std::size_t i = 0; i < sample.states.size(); ++i) {
        started_[i] = static_cast<char>(started_[i] != 0 || sample.states[i].started);
        ended_[i] = static_cast<char>(ended_[i] != 0 || sample.states[i].ended);
    }
    latest_ = sample;
}

ActionSample StepLatch::take(const ActionMap& map) {
    ActionSample out = latest_;
    for (std::size_t i = 0; i < out.states.size() && i < started_.size(); ++i) {
        ActionState& s = out.states[i];
        s.started = started_[i] != 0;
        s.ended = ended_[i] != 0;
        // A one-frame trigger that fired since the last take is active for
        // this one, even when the sample it fired in was never taken.
        if (i < map.actions.size() && one_frame(map.actions[i].trigger.kind)) {
            s.active = s.active || s.started;
        }
        started_[i] = 0;
        ended_[i] = 0;
    }
    return out;
}

std::expected<void, core::Error> Actions::inject(std::string_view name, glm::vec2 value, int frames) {
    const int i = find(name);
    if (i < 0) return std::unexpected(core::Error{"unknown action '" + std::string(name) + "'"});
    if (frames < 1 || frames > k_max_inject_frames) {
        return std::unexpected(core::Error{"frames: expected 1.." + std::to_string(k_max_inject_frames)});
    }
    if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
        return std::unexpected(core::Error{"value: expected finite numbers"});
    }
    clear_injection(name);
    injections_.push_back(Injection{i, value, frames});
    return {};
}

void Actions::clear_injection(std::string_view name) {
    const int i = find(name);
    std::erase_if(injections_, [i](const Injection& inj) { return inj.action == i; });
}

} // namespace engine::input
