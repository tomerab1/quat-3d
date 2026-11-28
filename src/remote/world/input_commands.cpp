// input.* (ROADMAP G5): list actions and contexts, inject action values (tests,
// playtests over MCP), rebind, and push / pop contexts. See
// docs/remote-control.md and docs/input.md.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/input/actions.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr int k_max_frames = 36000;

[[nodiscard]] Json action_json(const input::ActionDesc& a, const input::ActionState& s) {
    Json bindings = Json::array();
    for (const input::Binding& b : a.bindings) bindings.push_back(input::binding_json(b));
    Json trigger = input::action_map_json(input::ActionMap{{}, {a}})["actions"][0]["trigger"];
    return Json{{"name", a.name},
                {"type", std::string(input::action_type_name(a.type))},
                {"context", a.context},
                {"trigger", std::move(trigger)},
                {"bindings", std::move(bindings)},
                {"value", Json::array({s.value.x, s.value.y})},
                {"active", s.active},
                {"actuated", s.actuated},
                {"held_s", s.held_s}};
}

[[nodiscard]] Json contexts_json(const input::Actions& actions) {
    Json contexts = Json::array();
    for (const input::ContextDesc& c : actions.map().contexts) {
        contexts.push_back({{"name", c.name},
                            {"on_stack", actions.context_active(c.name)},
                            {"exclusive", c.exclusive},
                            {"consume", c.consume}});
    }
    Json stack = Json::array();
    for (const int i : actions.context_stack()) stack.push_back(actions.map().contexts[static_cast<std::size_t>(i)].name);
    return Json{{"contexts", std::move(contexts)}, {"stack", std::move(stack)}};
}

[[nodiscard]] std::expected<int, RpcError> action_index(const input::Actions& actions, core::JsonFields& f) {
    std::string name;
    if (!f.has("action")) f.fail("action", "missing (an action name; input.actions lists them)");
    f.string("action", name, 64);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    const int i = actions.find(name);
    if (i < 0) {
        std::string known;
        for (const input::ActionDesc& a : actions.map().actions) known += (known.empty() ? "" : ", ") + a.name;
        return bad_params("action: unknown action '" + name + "' (known: " + known + ")");
    }
    return i;
}

RpcResult input_actions(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::string only;
    f.string("action", only, 64);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (ctx.services.actions == nullptr) return unavailable("input actions");
    const input::Actions& a = *ctx.services.actions;
    Json out = contexts_json(a);
    Json list = Json::array();
    for (std::size_t i = 0; i < a.map().actions.size(); ++i) {
        const input::ActionDesc& d = a.map().actions[i];
        if (!only.empty() && d.name != only) continue;
        list.push_back(action_json(d, a.frame().states[i]));
    }
    if (!only.empty() && list.empty()) return bad_params("action: unknown action '" + only + "'");
    out["actions"] = std::move(list);
    return out;
}

// {action, value: number | [x, y] | true | false, frames?=1} or {action, clear: true}.
RpcResult input_inject(Context& ctx, const Json& params) {
    if (ctx.services.actions == nullptr) return unavailable("input actions");
    input::Actions& a = *ctx.services.actions;
    core::JsonFields f(params, "");
    const Json* value = f.raw("value");
    bool clear = false;
    int frames = 1;
    f.boolean("clear", clear).integer("frames", frames, 1, k_max_frames);
    auto index = action_index(a, f);
    if (!index) return std::unexpected(index.error());
    const input::ActionDesc& d = a.map().actions[static_cast<std::size_t>(*index)];
    if (clear) {
        if (value != nullptr) return bad_params("give either value or clear, not both");
        a.clear_injection(d.name);
        return Json{{"action", d.name}, {"cleared", true}};
    }
    glm::vec2 v(0.0F);
    bool ok = value != nullptr;
    if (ok && value->is_boolean()) {
        v.x = value->get<bool>() ? 1.0F : 0.0F;
    } else if (ok && value->is_array() && value->size() == 2) {
        ok = core::JsonFields::read_float((*value)[0], v.x) && core::JsonFields::read_float((*value)[1], v.y);
    } else if (ok) {
        ok = core::JsonFields::read_float(*value, v.x);
    }
    if (!ok || glm::any(glm::greaterThan(glm::abs(v), glm::vec2(1.0e5F)))) {
        return bad_params("value: expected true / false (button), a number (axis1d) or [x, y] (axis2d)");
    }
    if (d.type == input::ActionType::axis2d && value->is_number()) {
        return bad_params("value: '" + d.name + "' is an axis2d action: give [x, y]");
    }
    if (auto r = a.inject(d.name, v, frames); !r) return bad_params(r.error().message);
    return Json{{"action", d.name}, {"value", Json::array({v.x, v.y})}, {"frames", frames}};
}

// {action, bindings: [...] | null (back to the map's), save?=true}.
RpcResult input_bind(Context& ctx, const Json& params) {
    if (ctx.services.actions == nullptr) return unavailable("input actions");
    input::Actions& a = *ctx.services.actions;
    core::JsonFields f(params, "");
    const Json* bindings = f.raw("bindings");
    bool save = true;
    f.boolean("save", save);
    if (bindings == nullptr) f.fail("bindings", "missing (an array of bindings, or null to reset)");
    auto index = action_index(a, f);
    if (!index) return std::unexpected(index.error());
    const std::string name = a.map().actions[static_cast<std::size_t>(*index)].name;
    if (bindings->is_null()) {
        if (auto r = a.reset_bindings(name); !r) return bad_params(r.error().message);
    } else {
        if (!bindings->is_array() || bindings->size() > input::k_max_bindings) {
            return bad_params("bindings: expected an array of at most " + std::to_string(input::k_max_bindings) + " bindings");
        }
        std::vector<input::Binding> parsed;
        for (std::size_t i = 0; i < bindings->size(); ++i) {
            auto b = input::parse_binding((*bindings)[i], "bindings[" + std::to_string(i) + "]");
            if (!b) return bad_params(b.error().message);
            parsed.push_back(std::move(*b));
        }
        if (auto r = a.rebind(name, std::move(parsed)); !r) return bad_params(r.error().message);
    }
    bool saved = false;
    if (save && ctx.services.save_input_bindings) {
        if (auto r = ctx.services.save_input_bindings(); !r) {
            return rpc_fail(rpc_code::failed, "rebound, but the user file was not written: " + r.error().message);
        }
        saved = true;
    }
    const auto i = static_cast<std::size_t>(*index);
    Json out = action_json(a.map().actions[i], a.frame().states[i]);
    out["saved"] = saved;
    return out;
}

// {push: name} | {pop: name}.
RpcResult input_context(Context& ctx, const Json& params) {
    if (ctx.services.actions == nullptr) return unavailable("input actions");
    input::Actions& a = *ctx.services.actions;
    core::JsonFields f(params, "");
    std::string push;
    std::string pop;
    f.string("push", push, 64).string("pop", pop, 64);
    if (push.empty() == pop.empty()) f.fail("push", "give exactly one of push or pop (a context name)");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!push.empty()) {
        if (auto r = a.push_context(push); !r) return bad_params("push: " + r.error().message);
    } else if (!a.pop_context(pop)) {
        return bad_params("pop: context '" + pop + "' is not on the stack");
    }
    return contexts_json(a);
}

} // namespace

void register_input_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("input.actions",
                      "Input actions and contexts: {action?} -> {contexts, stack, actions: [{name, type, "
                      "context, trigger, bindings, value, active, actuated, held_s}]}.",
                      [ctx](const Json& p) { return input_actions(*ctx, p); });
    server.add_method("input.inject",
                      "Make an action read a value for some frames: {action, value (bool | number | [x, y]), "
                      "frames?=1} or {action, clear: true}.",
                      [ctx](const Json& p) { return input_inject(*ctx, p); });
    server.add_method("input.bind",
                      "Rebind an action: {action, bindings: [...] | null (map default), save?=true (the "
                      "user's file)}.",
                      [ctx](const Json& p) { return input_bind(*ctx, p); });
    server.add_method("input.context", "Push or pop an input context: {push | pop: name} -> {contexts, stack}.",
                      [ctx](const Json& p) { return input_context(*ctx, p); });
}

} // namespace engine::remote::world
