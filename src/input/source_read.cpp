// Reading bindings from a device state: sources, composites, chords and
// consumption — see include/engine/input/actions.hpp.

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/input/device_state.hpp"
#include "input_internal.hpp"

namespace engine::input {

namespace {

constexpr std::size_t k_left_ctrl = 224, k_left_shift = 225, k_left_alt = 226;
constexpr std::size_t k_right_ctrl = 228, k_right_shift = 229, k_right_alt = 230;

[[nodiscard]] bool key_held(const DeviceState& d, std::size_t k) {
    return k < DeviceState::key_count && d.keys.test(k);
}

// Gamepad sticks share the axes: stick 0 is axes 0/1, stick 1 axes 2/3.
[[nodiscard]] std::size_t stick_axis(std::uint16_t stick, int component) {
    return static_cast<std::size_t>(stick) * 2U + static_cast<std::size_t>(component);
}

[[nodiscard]] glm::vec2 edge_direction(const DeviceState& d, const Source& s) {
    if (!d.focused || (!d.fullscreen && !s.edge_windowed)) return glm::vec2(0.0F);
    const glm::vec2 size = d.viewport_size;
    const glm::vec2 p = d.pointer();
    if (size.x <= 0.0F || size.y <= 0.0F || p.x < 0.0F || p.y < 0.0F || p.x > size.x ||
        p.y > size.y) {
        return glm::vec2(0.0F); // outside the viewport: no scrolling
    }
    glm::vec2 v(0.0F);
    if (p.x < s.edge_px) v.x = -1.0F;
    if (p.x > size.x - s.edge_px) v.x = 1.0F;
    if (p.y < s.edge_px) v.y = 1.0F; // the top edge is "up" / forward
    if (p.y > size.y - s.edge_px) v.y = -1.0F;
    return v.x != 0.0F && v.y != 0.0F ? glm::normalize(v) : v; // corners are not faster

}

[[nodiscard]] glm::vec2 raw_source(const Source& s, const ReadContext& ctx) {
    const DeviceState& d = *ctx.device;
    if (ctx.consumed->has(s)) return glm::vec2(0.0F);
    if (is_pointer_control(s.control) && !ctx.pointer_allowed) return glm::vec2(0.0F);
    const bool pad = d.gamepad_connected;
    switch (s.control) {
    case Control::none: return glm::vec2(0.0F);
    case Control::key: {
        if (d.keyboard_blocked || !key_held(d, s.index)) return glm::vec2(0.0F);
        if ((held_chord(d) & s.chord) != s.chord) return glm::vec2(0.0F);
        // A satisfied chord with more modifiers on this key wins.
        const int mine = std::popcount(static_cast<unsigned>(s.chord));
        if (ctx.chord_best != nullptr && (*ctx.chord_best)[s.index] > mine) return glm::vec2(0.0F);
        return glm::vec2(1.0F, 0.0F);
    }
    case Control::mouse_button:
        return glm::vec2(s.index < DeviceState::mouse_button_count && d.mouse.test(s.index) ? 1.0F : 0.0F, 0.0F);
    case Control::mouse_delta: return d.mouse_delta;
    case Control::mouse_wheel: return glm::vec2(d.mouse_wheel, 0.0F);
    case Control::pointer: return d.pointer();
    case Control::edge: return edge_direction(d, s);
    case Control::gamepad_button:
        return glm::vec2(pad && s.index < DeviceState::gamepad_button_count && d.gamepad.test(s.index) ? 1.0F : 0.0F, 0.0F);
    case Control::gamepad_axis:
        return glm::vec2(pad && s.index < DeviceState::gamepad_axis_count ? d.gamepad_axes[s.index] : 0.0F, 0.0F);
    case Control::gamepad_stick:
        if (!pad || s.index > 1) return glm::vec2(0.0F);
        // SDL reports y down; sticks read up positive.
        return {d.gamepad_axes[stick_axis(s.index, 0)], -d.gamepad_axes[stick_axis(s.index, 1)]};
    }
    return glm::vec2(0.0F);
}

// The source's value, one component taken when it asks for one (into x).
[[nodiscard]] glm::vec2 read_source(const Source& s, const ReadContext& ctx) {
    const glm::vec2 v = raw_source(s, ctx);
    switch (s.component) {
    case Component::both: return v;
    case Component::x: return glm::vec2(v.x, 0.0F);
    case Component::y: return glm::vec2(v.y, 0.0F);
    }
    return v;
}

[[nodiscard]] float part(const Source& s, const ReadContext& ctx) {
    if (!s.valid()) return 0.0F;
    return std::clamp(read_source(s, ctx).x, 0.0F, 1.0F);
}

} // namespace

bool is_pointer_control(Control c) {
    return c == Control::mouse_button || c == Control::mouse_delta || c == Control::mouse_wheel ||
           c == Control::pointer || c == Control::edge;
}

std::uint8_t held_chord(const DeviceState& d) {
    std::uint8_t bits = 0;
    if (key_held(d, k_left_ctrl) || key_held(d, k_right_ctrl)) bits |= chord_ctrl;
    if (key_held(d, k_left_shift) || key_held(d, k_right_shift)) bits |= chord_shift;
    if (key_held(d, k_left_alt) || key_held(d, k_right_alt)) bits |= chord_alt;
    return bits;
}

void chord_priorities(const DeviceState& d, std::span<const ActionDesc* const> actions,
                      std::array<std::uint8_t, DeviceState::key_count>& best) {
    best.fill(0);
    if (d.keyboard_blocked) return;
    const std::uint8_t held = held_chord(d);
    const auto note = [&](const Source& s) {
        if (s.control != Control::key || s.chord == 0 || !key_held(d, s.index)) return;
        if ((held & s.chord) != s.chord) return;
        const auto n = static_cast<std::uint8_t>(std::popcount(static_cast<unsigned>(s.chord)));
        best[s.index] = std::max(best[s.index], n);
    };
    for (const ActionDesc* a : actions) {
        for (const Binding& b : a->bindings) {
            note(b.source);
            for (const Source& p : b.parts) note(p);
        }
    }
}

bool Consumed::has(const Source& s) const {
    if (all) return true;
    switch (s.control) {
    case Control::none: return false;
    case Control::key: return s.index < keys.size() && keys.test(s.index);
    case Control::mouse_button: return s.index < mouse.size() && mouse.test(s.index);
    case Control::mouse_delta: return delta;
    case Control::mouse_wheel: return wheel;
    case Control::pointer:
    case Control::edge: return pointer;
    case Control::gamepad_button: return s.index < pad_buttons.size() && pad_buttons.test(s.index);
    case Control::gamepad_axis: return s.index < pad_axes.size() && pad_axes.test(s.index);
    case Control::gamepad_stick:
        return s.index <= 1 && (pad_axes.test(stick_axis(s.index, 0)) || pad_axes.test(stick_axis(s.index, 1)));
    }
    return false;
}

void Consumed::add(const Source& s) {
    switch (s.control) {
    case Control::none: break;
    case Control::key: if (s.index < keys.size()) keys.set(s.index); break;
    case Control::mouse_button: if (s.index < mouse.size()) mouse.set(s.index); break;
    case Control::mouse_delta: delta = true; break;
    case Control::mouse_wheel: wheel = true; break;
    case Control::pointer:
    case Control::edge: pointer = true; break;
    case Control::gamepad_button: if (s.index < pad_buttons.size()) pad_buttons.set(s.index); break;
    case Control::gamepad_axis: if (s.index < pad_axes.size()) pad_axes.set(s.index); break;
    case Control::gamepad_stick:
        if (s.index <= 1) {
            pad_axes.set(stick_axis(s.index, 0));
            pad_axes.set(stick_axis(s.index, 1));
        }
        break;
    }
}

void apply_modifier(const Modifier& m, glm::vec2& v) {
    switch (m.kind) {
    case ModifierKind::dead_zone: {
        // Radial: the magnitude is remapped from [lo, hi] to [0, 1], the
        // direction kept (an axial dead zone on a stick makes it snap to
        // the axes).
        const float len = glm::length(v);
        if (len <= m.value.x) {
            v = glm::vec2(0.0F);
        } else {
            const float t = std::min((len - m.value.x) / (m.value.y - m.value.x), 1.0F);
            v *= t / len;
        }
        break;
    }
    case ModifierKind::scale: v *= m.value; break;
    case ModifierKind::invert: v *= glm::vec2(m.value.x != 0.0F ? -1.0F : 1.0F, m.value.y != 0.0F ? -1.0F : 1.0F); break;
    case ModifierKind::smoothing:
    case ModifierKind::camera_relative: break; // stateful / per action (actions.cpp)
    }
}

glm::vec2 read_binding(const Binding& b, const ReadContext& ctx, bool& pointer_used) {
    glm::vec2 v(0.0F);
    switch (b.kind) {
    case Binding::Kind::single: v = read_source(b.source, ctx); break;
    case Binding::Kind::composite1d: v = glm::vec2(part(b.parts[1], ctx) - part(b.parts[0], ctx), 0.0F); break;
    case Binding::Kind::composite2d:
        v = {part(b.parts[3], ctx) - part(b.parts[2], ctx), part(b.parts[0], ctx) - part(b.parts[1], ctx)};
        // Digital composites: a diagonal is as fast as a straight line.
        if (glm::length(v) > 1.0F) v = glm::normalize(v);
        break;
    }
    if (b.with.valid() && std::abs(read_source(b.with, ctx).x) < 0.5F) v = glm::vec2(0.0F);
    for (const Modifier& m : b.modifiers) apply_modifier(m, v);
    if (v != glm::vec2(0.0F)) {
        bool pointer = is_pointer_control(b.source.control) || is_pointer_control(b.with.control);
        for (const Source& p : b.parts) pointer = pointer || is_pointer_control(p.control);
        pointer_used = pointer_used || pointer;
    }
    return v;
}

void consume_binding(const Binding& b, Consumed& consumed) {
    consumed.add(b.source);
    for (const Source& p : b.parts) consumed.add(p);
    consumed.add(b.with);
}

} // namespace engine::input
