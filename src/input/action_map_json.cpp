// Action maps: JSON codec, lookup and merging — see
// include/engine/input/action_map.hpp and docs/input.md.

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/input/action_map.hpp"
#include "input_internal.hpp"

namespace engine::input {

namespace {

using Json = nlohmann::json;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<ActionType> k_types[] = {{"button", ActionType::button},
                                            {"axis1d", ActionType::axis1d},
                                            {"axis2d", ActionType::axis2d}};

struct TriggerName {
    std::string_view name;
    TriggerKind      kind;
    float            default_param;
    float            lo;
    float            hi;
};
constexpr std::array k_triggers = {
    TriggerName{"down", TriggerKind::down, 0.0F, 0.0F, 0.0F},
    TriggerName{"pressed", TriggerKind::pressed, 0.0F, 0.0F, 0.0F},
    TriggerName{"released", TriggerKind::released, 0.0F, 0.0F, 0.0F},
    TriggerName{"tap", TriggerKind::tap, 0.25F, 0.01F, 5.0F},
    TriggerName{"hold", TriggerKind::hold, 0.5F, 0.01F, 30.0F},
    TriggerName{"double_click", TriggerKind::double_click, 0.3F, 0.05F, 2.0F},
    TriggerName{"drag", TriggerKind::drag, 4.0F, 0.0F, 256.0F},
};

constexpr std::size_t k_max_name = 64;

// Lowercase letters, digits, '_', '.', '-' (names appear in Lua, JSON and
// tool calls).
[[nodiscard]] bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > k_max_name) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

void read_name(JsonFields& f, std::string& out) {
    f.string("name", out, k_max_name);
    if (f.ok() && !valid_name(out)) {
        f.fail("name", "expected 1-64 characters of a-z, 0-9, '_', '.', '-'");
    }
}

void read_trigger(JsonFields& f, Trigger& out) {
    const Json* v = f.raw("trigger");
    if (v == nullptr || v->is_null()) return;
    std::string name;
    const Json* arg = nullptr;
    if (v->is_string()) {
        name = v->get<std::string>();
    } else if (v->is_object() && v->size() == 1) {
        name = v->begin().key();
        arg = &v->begin().value();
    } else {
        f.fail("trigger", "expected a trigger name or {\"<name>\": <parameter>}");
        return;
    }
    for (const TriggerName& t : k_triggers) {
        if (t.name != name) continue;
        out.kind = t.kind;
        out.param = t.default_param;
        if (arg != nullptr) {
            float p = 0.0F;
            if (t.hi == 0.0F || !JsonFields::read_float(*arg, p) || p < t.lo || p > t.hi) {
                f.fail("trigger", t.hi == 0.0F
                                      ? "'" + name + "' takes no parameter"
                                      : "'" + name + "': expected a number in [" +
                                            std::to_string(t.lo) + ", " + std::to_string(t.hi) + "]");
                return;
            }
            out.param = p;
        }
        return;
    }
    f.fail("trigger", "unknown trigger '" + name + "' (down, pressed, released, tap, hold, double_click, drag)");
}

[[nodiscard]] std::expected<ContextDesc, core::Error> parse_context(const Json& json,
                                                                    const std::string& at) {
    ContextDesc c;
    JsonFields f(json, at);
    read_name(f, c.name);
    f.boolean("active", c.active).boolean("exclusive", c.exclusive).boolean("consume", c.consume);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    return c;
}

[[nodiscard]] std::expected<ActionDesc, core::Error> parse_action(const Json& json,
                                                                  const std::string& at) {
    ActionDesc a;
    JsonFields f(json, at);
    read_name(f, a.name);
    if (!f.has("type")) f.fail("type", "missing (button, axis1d or axis2d)");
    f.enumeration("type", a.type, k_types);
    f.string("context", a.context, k_max_name);
    if (f.ok() && !valid_name(a.context)) f.fail("context", "expected a context name");
    read_modifiers(f, "modifiers", a.modifiers, true);
    read_trigger(f, a.trigger);
    f.number("press_point", a.press_point, 0.01F, 1.0F);
    const Json* bindings = f.raw("bindings");
    if (bindings != nullptr && !(bindings->is_array() && bindings->size() <= k_max_bindings)) {
        f.fail("bindings", "expected an array of at most " + std::to_string(k_max_bindings) + " bindings");
    }
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    for (std::size_t i = 0; bindings != nullptr && i < bindings->size(); ++i) {
        auto b = parse_binding((*bindings)[i], at + ".bindings[" + std::to_string(i) + "]");
        if (!b) return std::unexpected(b.error());
        a.bindings.push_back(std::move(*b));
    }
    for (const Modifier& m : a.modifiers) {
        if (m.kind == ModifierKind::camera_relative && a.type != ActionType::axis2d) {
            return std::unexpected(core::Error{at + ".modifiers: camera_relative needs an axis2d action"});
        }
    }
    return a;
}

[[nodiscard]] Json trigger_json(const Trigger& t) {
    for (const TriggerName& n : k_triggers) {
        if (n.kind != t.kind) continue;
        if (n.hi == 0.0F) return std::string(n.name);
        return Json{{std::string(n.name), t.param}};
    }
    return "down";
}

[[nodiscard]] std::expected<void, core::Error> check_contexts(const ActionMap& map) {
    for (const ActionDesc& a : map.actions) {
        if (map.find_context(a.context) >= 0) continue;
        std::string known;
        for (const ContextDesc& c : map.contexts) known += (known.empty() ? "" : ", ") + c.name;
        return std::unexpected(core::Error{"action '" + a.name + "': unknown context '" + a.context +
                                           "' (declared: " + known + ")"});
    }
    return {};
}

void ensure_default_context(ActionMap& map) {
    if (map.find_context("default") < 0) {
        map.contexts.insert(map.contexts.begin(), ContextDesc{"default", true, false, true});
    }
}

} // namespace

std::string_view action_type_name(ActionType type) {
    for (const auto& t : k_types) {
        if (t.value == type) return t.name;
    }
    return "button";
}

int ActionMap::find_action(std::string_view name) const {
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int ActionMap::find_context(std::string_view name) const {
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (contexts[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::expected<ActionMap, core::Error> parse_action_map(const Json& json) {
    ActionMap map;
    JsonFields f(json, "");
    int version = 1;
    f.integer("version", version, 1, 1);
    const Json* contexts = f.raw("contexts");
    const Json* actions = f.raw("actions");
    if (contexts != nullptr && !(contexts->is_array() && contexts->size() <= k_max_contexts)) {
        f.fail("contexts", "expected an array of at most " + std::to_string(k_max_contexts) + " contexts");
    }
    if (actions == nullptr) f.fail("actions", "missing (an array of actions)");
    if (actions != nullptr && !(actions->is_array() && actions->size() <= k_max_actions)) {
        f.fail("actions", "expected an array of at most " + std::to_string(k_max_actions) + " actions");
    }
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());

    for (std::size_t i = 0; contexts != nullptr && i < contexts->size(); ++i) {
        auto c = parse_context((*contexts)[i], "contexts[" + std::to_string(i) + "]");
        if (!c) return std::unexpected(c.error());
        if (map.find_context(c->name) >= 0) {
            return std::unexpected(core::Error{"contexts[" + std::to_string(i) + "]: duplicate context '" + c->name + "'"});
        }
        map.contexts.push_back(std::move(*c));
    }
    ensure_default_context(map);
    for (std::size_t i = 0; i < actions->size(); ++i) {
        auto a = parse_action((*actions)[i], "actions[" + std::to_string(i) + "]");
        if (!a) return std::unexpected(a.error());
        if (map.find_action(a->name) >= 0) {
            return std::unexpected(core::Error{"actions[" + std::to_string(i) + "]: duplicate action '" + a->name + "'"});
        }
        map.actions.push_back(std::move(*a));
    }
    if (auto ok = check_contexts(map); !ok) return std::unexpected(ok.error());
    return map;
}

Json action_map_json(const ActionMap& map) {
    Json contexts = Json::array();
    for (const ContextDesc& c : map.contexts) {
        contexts.push_back({{"name", c.name},
                            {"active", c.active},
                            {"exclusive", c.exclusive},
                            {"consume", c.consume}});
    }
    Json actions = Json::array();
    for (const ActionDesc& a : map.actions) {
        Json bindings = Json::array();
        for (const Binding& b : a.bindings) bindings.push_back(binding_json(b));
        Json j{{"name", a.name},
               {"type", std::string(action_type_name(a.type))},
               {"context", a.context},
               {"bindings", std::move(bindings)},
               {"trigger", trigger_json(a.trigger)},
               {"press_point", a.press_point}};
        if (!a.modifiers.empty()) j["modifiers"] = modifiers_json(a.modifiers);
        actions.push_back(std::move(j));
    }
    return Json{{"version", 1}, {"contexts", std::move(contexts)}, {"actions", std::move(actions)}};
}

std::expected<void, core::Error> merge_action_map(ActionMap& base, ActionMap extra) {
    ActionMap out = base;
    for (ContextDesc& c : extra.contexts) {
        const int i = out.find_context(c.name);
        if (i >= 0) {
            out.contexts[static_cast<std::size_t>(i)] = std::move(c);
        } else {
            out.contexts.push_back(std::move(c));
        }
    }
    for (ActionDesc& a : extra.actions) {
        const int i = out.find_action(a.name);
        if (i >= 0) {
            out.actions[static_cast<std::size_t>(i)] = std::move(a);
        } else {
            out.actions.push_back(std::move(a));
        }
    }
    if (out.actions.size() > k_max_actions || out.contexts.size() > k_max_contexts) {
        return std::unexpected(core::Error{"merged action map exceeds " + std::to_string(k_max_actions) +
                                           " actions or " + std::to_string(k_max_contexts) + " contexts"});
    }
    if (auto ok = check_contexts(out); !ok) return std::unexpected(ok.error());
    base = std::move(out);
    return {};
}

} // namespace engine::input
