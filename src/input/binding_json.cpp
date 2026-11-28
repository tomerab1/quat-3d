// Sources, modifiers and bindings of action maps, to and from JSON — see
// include/engine/input/action_map.hpp and docs/input.md.

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/input/action_map.hpp"
#include "engine/input/device_state.hpp"
#include "input_internal.hpp"

namespace engine::input {

namespace {

using Json = nlohmann::json;
using core::JsonFields;

struct Chord {
    std::string_view name;
    std::uint8_t     bit;
};
constexpr std::array k_chords = {Chord{"ctrl", chord_ctrl}, Chord{"shift", chord_shift},
                                 Chord{"alt", chord_alt}};

[[nodiscard]] Component suffix(std::string_view& name) {
    if (name.ends_with("_x")) {
        name.remove_suffix(2);
        return Component::x;
    }
    if (name.ends_with("_y")) {
        name.remove_suffix(2);
        return Component::y;
    }
    return Component::both;
}

[[nodiscard]] std::expected<Source, std::string> parse_key(std::string_view rest) {
    Source s;
    s.control = Control::key;
    // Chord prefixes: "ctrl+shift+z". A lone "+" is not a key name we have.
    while (true) {
        const std::size_t plus = rest.find('+');
        if (plus == std::string_view::npos) break;
        const std::string_view mod = rest.substr(0, plus);
        bool known = false;
        for (const Chord& c : k_chords) {
            if (c.name == mod) {
                s.chord |= c.bit;
                known = true;
            }
        }
        if (!known) return std::unexpected("unknown chord modifier '" + std::string(mod) + "' (ctrl, shift, alt)");
        rest.remove_prefix(plus + 1);
    }
    const auto k = key_from_name(rest);
    if (!k) return std::unexpected("unknown key '" + std::string(rest) + "' (a-z, 0-9, f1-f12, space, enter, left_shift, ... see docs/input.md)");
    s.index = static_cast<std::uint16_t>(*k);
    return s;
}

[[nodiscard]] std::expected<Source, std::string> parse_mouse(std::string_view rest) {
    Source s;
    if (rest == "wheel") {
        s.control = Control::mouse_wheel;
        return s;
    }
    std::string_view base = rest;
    const Component c = suffix(base);
    if (base == "delta") {
        s.control = Control::mouse_delta;
        s.component = c;
        return s;
    }
    const auto b = mouse_button_from_name(rest);
    if (!b) return std::unexpected("unknown mouse control '" + std::string(rest) + "' (left, middle, right, x1, x2, delta, delta_x, delta_y, wheel)");
    s.control = Control::mouse_button;
    s.index = static_cast<std::uint16_t>(*b);
    return s;
}

[[nodiscard]] std::expected<Source, std::string> parse_gamepad(std::string_view rest) {
    Source s;
    if (const auto b = gamepad_button_from_name(rest)) {
        s.control = Control::gamepad_button;
        s.index = static_cast<std::uint16_t>(*b);
        return s;
    }
    if (const auto a = gamepad_axis_from_name(rest)) {
        s.control = Control::gamepad_axis;
        s.index = static_cast<std::uint16_t>(*a);
        return s;
    }
    std::string_view base = rest;
    const Component c = suffix(base);
    if (c == Component::both && (base.ends_with("_xy"))) base.remove_suffix(3);
    if ((base == "left_stick" || base == "right_stick") && rest != base) {
        s.control = Control::gamepad_stick;
        s.index = base == "left_stick" ? 0 : 1;
        s.component = c;
        return s;
    }
    return std::unexpected("unknown gamepad control '" + std::string(rest) + "' (south/a, east/b, west/x, north/y, start, dpad_up, left_x, left_trigger, left_stick_xy, right_stick_xy, ...)");
}

} // namespace

std::expected<Source, std::string> parse_source(std::string_view text) {
    const std::size_t colon = text.find(':');
    const std::string_view device = text.substr(0, colon);
    const std::string_view rest =
        colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
    if (device == "key" && !rest.empty()) return parse_key(rest);
    if (device == "mouse" && !rest.empty()) return parse_mouse(rest);
    if (device == "gamepad" && !rest.empty()) return parse_gamepad(rest);
    std::string_view base = device;
    const Component c = suffix(base);
    if (colon == std::string_view::npos && (base == "pointer" || base == "edge")) {
        Source s;
        s.control = base == "pointer" ? Control::pointer : Control::edge;
        s.component = c;
        return s;
    }
    return std::unexpected("unknown source '" + std::string(text) + "' (key:<name>, key:ctrl+<name>, mouse:<button|delta|wheel>, pointer, edge, gamepad:<button|axis|left_stick_xy|right_stick_xy>)");
}

std::string source_string(const Source& s) {
    std::string out;
    const auto comp = [&] {
        if (s.component == Component::x) out += "_x";
        if (s.component == Component::y) out += "_y";
    };
    switch (s.control) {
    case Control::none: break;
    case Control::key:
        out = "key:";
        for (const Chord& c : k_chords) {
            if ((s.chord & c.bit) != 0) out += std::string(c.name) + "+";
        }
        out += key_name(s.index);
        break;
    case Control::mouse_button: out = "mouse:" + std::string(mouse_button_name(s.index)); break;
    case Control::mouse_delta: out = "mouse:delta"; comp(); break;
    case Control::mouse_wheel: out = "mouse:wheel"; break;
    case Control::pointer: out = "pointer"; comp(); break;
    case Control::edge: out = "edge"; comp(); break;
    case Control::gamepad_button: out = "gamepad:" + std::string(gamepad_button_name(s.index)); break;
    case Control::gamepad_axis: out = "gamepad:" + std::string(gamepad_axis_name(s.index)); break;
    case Control::gamepad_stick:
        out = s.index == 0 ? "gamepad:left_stick" : "gamepad:right_stick";
        if (s.component == Component::both) out += "_xy";
        comp();
        break;
    }
    return out;
}

bool is_2d(const Source& s) {
    return s.component == Component::both &&
           (s.control == Control::mouse_delta || s.control == Control::pointer ||
            s.control == Control::edge || s.control == Control::gamepad_stick);
}

// ---- Modifiers ----------------------------------------------------------------

void read_modifiers(JsonFields& f, std::string_view key, std::vector<Modifier>& out,
                    bool action_level) {
    const Json* v = f.raw(key);
    if (v == nullptr || v->is_null()) return;
    if (!v->is_array() || v->size() > k_max_modifiers) {
        f.fail(key, "expected an array of at most " + std::to_string(k_max_modifiers) + " modifiers");
        return;
    }
    std::vector<Modifier> mods;
    for (std::size_t i = 0; i < v->size() && f.ok(); ++i) {
        const Json& m = (*v)[i];
        const std::string at = std::string(key) + "[" + std::to_string(i) + "]";
        // String shorthands for the flag modifiers.
        if (m.is_string()) {
            const auto& s = m.get_ref<const std::string&>();
            if (s == "invert") {
                mods.push_back({ModifierKind::invert, glm::vec2(1.0F)});
            } else if (s == "camera_relative" && action_level) {
                mods.push_back({ModifierKind::camera_relative, glm::vec2(0.0F)});
            } else {
                f.fail(at, "unknown modifier '" + s + "' (dead_zone, scale, invert, smoothing, camera_relative)");
            }
            continue;
        }
        if (!m.is_object() || m.size() != 1) {
            f.fail(at, "expected an object with one modifier, e.g. {\"dead_zone\": 0.15}");
            continue;
        }
        const std::string& name = m.begin().key();
        const Json& arg = m.begin().value();
        Modifier mod;
        float a = 0.0F;
        float b = 0.0F;
        const bool scalar = JsonFields::read_float(arg, a);
        const bool pair = arg.is_array() && arg.size() == 2 && JsonFields::read_float(arg[0], a) &&
                          JsonFields::read_float(arg[1], b);
        if (name == "dead_zone") {
            mod.kind = ModifierKind::dead_zone;
            mod.value = scalar ? glm::vec2(a, 1.0F) : glm::vec2(a, b);
            if (!(scalar || pair) || mod.value.x < 0.0F || mod.value.y > 1.0F || mod.value.x >= mod.value.y) {
                f.fail(at, "dead_zone: expected a lower bound in [0, 1) or [lower, upper] with 0 <= lower < upper <= 1");
            }
        } else if (name == "scale") {
            mod.kind = ModifierKind::scale;
            mod.value = scalar ? glm::vec2(a) : glm::vec2(a, b);
            if (!(scalar || pair) || glm::any(glm::greaterThan(glm::abs(mod.value), glm::vec2(1.0e4F)))) {
                f.fail(at, "scale: expected a number or [x, y] within [-10000, 10000]");
            }
        } else if (name == "invert") {
            mod.kind = ModifierKind::invert;
            const std::string axes = arg.is_string() ? arg.get<std::string>() : std::string();
            if (arg.is_boolean()) {
                mod.value = arg.get<bool>() ? glm::vec2(1.0F) : glm::vec2(0.0F);
            } else if (axes == "x" || axes == "y" || axes == "xy") {
                mod.value = glm::vec2(axes != "y" ? 1.0F : 0.0F, axes != "x" ? 1.0F : 0.0F);
            } else {
                f.fail(at, "invert: expected true or \"x\", \"y\", \"xy\"");
            }
        } else if (name == "smoothing" && action_level) {
            mod.kind = ModifierKind::smoothing;
            mod.value = glm::vec2(a, 0.0F);
            if (!scalar || a < 0.0F || a > 10.0F) f.fail(at, "smoothing: expected a half-life in [0, 10] seconds");
        } else if (name == "camera_relative" && action_level) {
            mod.kind = ModifierKind::camera_relative;
            if (!arg.is_boolean()) f.fail(at, "camera_relative: expected true or false");
            if (arg.is_boolean() && !arg.get<bool>()) continue;
        } else {
            f.fail(at, "unknown modifier '" + name + "' (" +
                           (action_level ? "dead_zone, scale, invert, smoothing, camera_relative"
                                         : "dead_zone, scale, invert; smoothing and camera_relative go on the action") +
                           ")");
        }
        mods.push_back(mod);
    }
    if (f.ok()) out = std::move(mods);
}

Json modifiers_json(const std::vector<Modifier>& mods) {
    Json out = Json::array();
    for (const Modifier& m : mods) {
        switch (m.kind) {
        case ModifierKind::dead_zone:
            out.push_back(m.value.y == 1.0F ? Json{{"dead_zone", m.value.x}}
                                            : Json{{"dead_zone", {m.value.x, m.value.y}}});
            break;
        case ModifierKind::scale:
            out.push_back(m.value.x == m.value.y ? Json{{"scale", m.value.x}}
                                                 : Json{{"scale", {m.value.x, m.value.y}}});
            break;
        case ModifierKind::invert: {
            const bool x = m.value.x != 0.0F;
            const bool y = m.value.y != 0.0F;
            out.push_back(Json{{"invert", x && y ? "xy" : (x ? "x" : "y")}});
            break;
        }
        case ModifierKind::smoothing: out.push_back(Json{{"smoothing", m.value.x}}); break;
        case ModifierKind::camera_relative: out.push_back(Json{{"camera_relative", true}}); break;
        }
    }
    return out;
}

// ---- Bindings -----------------------------------------------------------------

namespace {

void read_source(JsonFields& f, std::string_view key, Source& out, bool required) {
    const Json* v = f.raw(key);
    if (v == nullptr || v->is_null()) {
        if (required) f.fail(key, "missing (a source such as \"key:w\")");
        return;
    }
    if (!v->is_string() || v->get_ref<const std::string&>().size() > 128) {
        f.fail(key, "expected a source string such as \"key:w\" or \"mouse:left\"");
        return;
    }
    auto s = parse_source(v->get_ref<const std::string&>());
    if (!s) {
        f.fail(key, s.error());
        return;
    }
    out = *s;
}

} // namespace

std::expected<Binding, core::Error> parse_binding(const Json& json, std::string_view context) {
    Binding b;
    if (json.is_string()) {
        auto s = parse_source(json.get_ref<const std::string&>());
        if (!s) return std::unexpected(core::Error{std::string(context) + ": " + s.error()});
        b.source = *s;
        return b;
    }
    JsonFields f(json, std::string(context));
    f.known({"source", "composite", "negative", "positive", "up", "down", "left", "right", "with",
             "modifiers", "edge_px", "windowed"});
    std::string composite;
    f.string("composite", composite, 8);
    if (composite.empty()) {
        read_source(f, "source", b.source, true);
    } else if (composite == "1d") {
        b.kind = Binding::Kind::composite1d;
        read_source(f, "negative", b.parts[0], true);
        read_source(f, "positive", b.parts[1], true);
    } else if (composite == "2d") {
        b.kind = Binding::Kind::composite2d;
        read_source(f, "up", b.parts[0], false);
        read_source(f, "down", b.parts[1], false);
        read_source(f, "left", b.parts[2], false);
        read_source(f, "right", b.parts[3], false);
        bool any = false;
        for (const Source& p : b.parts) any = any || p.valid();
        if (f.ok() && !any) f.fail("composite", "a 2d composite needs at least one of up, down, left, right");
    } else {
        f.fail("composite", "expected \"1d\" or \"2d\"");
    }
    read_source(f, "with", b.with, false);
    read_modifiers(f, "modifiers", b.modifiers, false);
    if (b.source.control == Control::edge) {
        f.number("edge_px", b.source.edge_px, 1.0F, 512.0F);
        f.boolean("windowed", b.source.edge_windowed);
    } else if (f.has("edge_px") || f.has("windowed")) {
        f.fail(f.has("edge_px") ? "edge_px" : "windowed", "only an \"edge\" source has edge zones");
    }
    for (const Source& p : b.parts) {
        if (p.valid() && is_2d(p)) f.fail("composite", "composite parts must be 1D (use a _x / _y suffix): " + source_string(p));
    }
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    return b;
}

Json binding_json(const Binding& b) {
    const bool plain_edge = b.source.edge_px == Source{}.edge_px && !b.source.edge_windowed;
    if (b.kind == Binding::Kind::single && !b.with.valid() && b.modifiers.empty() &&
        (b.source.control != Control::edge || plain_edge)) {
        return source_string(b.source);
    }
    Json out = Json::object();
    if (b.kind == Binding::Kind::single) {
        out["source"] = source_string(b.source);
        if (b.source.control == Control::edge) {
            out["edge_px"] = b.source.edge_px;
            out["windowed"] = b.source.edge_windowed;
        }
    } else if (b.kind == Binding::Kind::composite1d) {
        out["composite"] = "1d";
        out["negative"] = source_string(b.parts[0]);
        out["positive"] = source_string(b.parts[1]);
    } else {
        out["composite"] = "2d";
        constexpr std::array<const char*, 4> k_dirs = {"up", "down", "left", "right"};
        for (std::size_t i = 0; i < 4; ++i) {
            if (b.parts[i].valid()) out[k_dirs[i]] = source_string(b.parts[i]);
        }
    }
    if (b.with.valid()) out["with"] = source_string(b.with);
    if (!b.modifiers.empty()) out["modifiers"] = modifiers_json(b.modifiers);
    return out;
}

} // namespace engine::input
