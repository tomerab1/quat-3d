// The `input` table scripts use — see docs/scripting.md ("Input").

#include <string>
#include <tuple>

#include "engine/input/actions.hpp"
#include "engine/script/script_input.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

const ScriptInput k_no_input{};

[[nodiscard]] const ScriptInput& input_of(const ApiContext& ctx) {
    return ctx.services.input != nullptr ? *ctx.services.input : k_no_input;
}

enum class Edge : std::uint8_t { held, pressed, released };

[[nodiscard]] bool edge(bool now, bool before, Edge e) {
    switch (e) {
    case Edge::held: return now;
    case Edge::pressed: return now && !before;
    case Edge::released: return !now && before;
    }
    return false;
}

void bind_keys(sol::table& t, lua_State* L, ApiContext* ctx) {
    const auto key = [ctx](Edge e) {
        return [ctx, e](const std::string& name) -> Result<bool> {
            const auto k = key_from_name(name);
            if (!k) return fail("unknown key '" + name + "' (a-z, 0-9, f1-f12, space, enter, ... see docs/scripting.md)");
            const ScriptInput& in = input_of(*ctx);
            if (in.keyboard_blocked) return false;
            return edge(in.keys.test(*k), in.keys_prev.test(*k), e);
        };
    };
    t.set("key", bind(L, key(Edge::held)));
    t.set("key_pressed", bind(L, key(Edge::pressed)));
    t.set("key_released", bind(L, key(Edge::released)));

    const auto mouse = [ctx](Edge e) {
        return [ctx, e](const std::string& name) -> Result<bool> {
            const auto b = mouse_button_from_name(name);
            if (!b) return fail("unknown mouse button '" + name + "' (left, middle, right, x1, x2)");
            const ScriptInput& in = input_of(*ctx);
            return edge(in.mouse.test(*b), in.mouse_prev.test(*b), e);
        };
    };
    t.set("mouse_button", bind(L, mouse(Edge::held)));
    t.set("mouse_pressed", bind(L, mouse(Edge::pressed)));
    t.set("mouse_released", bind(L, mouse(Edge::released)));
    t.set_function("mouse_position", [ctx] {
        const glm::vec2 p = input_of(*ctx).mouse_position;
        return std::make_tuple(p.x, p.y);
    });
    t.set_function("mouse_delta", [ctx] {
        const glm::vec2 d = input_of(*ctx).mouse_delta;
        return std::make_tuple(d.x, d.y);
    });
    t.set_function("mouse_wheel", [ctx] { return input_of(*ctx).mouse_wheel; });
}

void bind_gamepad(sol::table& t, lua_State* L, ApiContext* ctx) {
    t.set_function("gamepad_connected", [ctx] { return input_of(*ctx).gamepad_connected; });
    const auto button = [ctx](Edge e) {
        return [ctx, e](const std::string& name) -> Result<bool> {
            const auto b = gamepad_button_from_name(name);
            if (!b) return fail("unknown gamepad button '" + name + "' (south/a, east/b, west/x, north/y, start, ...)");
            const ScriptInput& in = input_of(*ctx);
            return in.gamepad_connected && edge(in.gamepad.test(*b), in.gamepad_prev.test(*b), e);
        };
    };
    t.set("gamepad_button", bind(L, button(Edge::held)));
    t.set("gamepad_pressed", bind(L, button(Edge::pressed)));
    t.set("gamepad_released", bind(L, button(Edge::released)));
    t.set("gamepad_axis", bind(L, [ctx](const std::string& name) -> Result<float> {
              const auto a = gamepad_axis_from_name(name);
              if (!a) return fail("unknown gamepad axis '" + name + "' (left_x, left_y, right_x, right_y, left_trigger, right_trigger)");
              const ScriptInput& in = input_of(*ctx);
              return in.gamepad_connected ? in.gamepad_axes[*a] : 0.0F;
          }));
}

// The action named `name` in this phase's sample (G5), or a failure listing
// the map's actions.
[[nodiscard]] Result<const input::ActionState*> action_state(const ApiContext& ctx,
                                                             const std::string& name,
                                                             const input::ActionDesc** desc = nullptr) {
    const input::Actions* svc = ctx.action_service;
    const int i = svc != nullptr ? svc->find(name) : -1;
    if (i < 0) {
        std::string known;
        if (svc != nullptr) {
            for (const input::ActionDesc& a : svc->map().actions) known += (known.empty() ? "" : ", ") + a.name;
        }
        return fail("unknown action '" + name + "' (" + (known.empty() ? "no action map" : known) + ")");
    }
    static const input::ActionState k_idle{};
    if (desc != nullptr) *desc = &svc->map().actions[static_cast<std::size_t>(i)];
    const input::ActionSample* sample = ctx.action_sample;
    if (sample == nullptr || static_cast<std::size_t>(i) >= sample->states.size()) return &k_idle;
    return &sample->states[static_cast<std::size_t>(i)];
}

void bind_virtual(sol::table& t, lua_State* L, ApiContext* ctx) {
    t.set("axis", bind(L, [ctx](const std::string& name) -> Result<float> {
              const input::ActionDesc* desc = nullptr;
              auto s = action_state(*ctx, name, &desc);
              if (!s.value) return std::unexpected(s.value.error());
              if (desc->type == input::ActionType::axis2d) {
                  return fail("action '" + name + "' is a 2D axis: read it with input.vector");
              }
              return (*s.value)->value.x;
          }));
    t.set("value", t.get<sol::object>("axis"));
    t.set("vector", bind(L, [ctx](const std::string& name) -> Result<glm::vec3> {
              auto s = action_state(*ctx, name);
              if (!s.value) return std::unexpected(s.value.error());
              const glm::vec2 v = (*s.value)->value;
              return glm::vec3(v.x, v.y, 0.0F);
          }));
    t.set("move_vector", bind(L, [ctx]() -> Result<glm::vec3> {
              auto x = action_state(*ctx, "move_x");
              auto y = action_state(*ctx, "move_y");
              if (!x.value || !y.value) return fail("the action map has no move_x / move_y actions");
              glm::vec3 v((*x.value)->value.x, 0.0F, -(*y.value)->value.x);
              if (glm::length(v) > 1.0F) v = glm::normalize(v);
              return v;
          }));
    const auto action = [ctx](int phase) {
        return [ctx, phase](const std::string& name) -> Result<bool> {
            auto s = action_state(*ctx, name);
            if (!s.value) return std::unexpected(s.value.error());
            const input::ActionState& st = **s.value;
            return phase == 0 ? st.active : (phase == 1 ? st.started : st.ended);
        };
    };
    t.set("action", bind(L, action(0)));
    t.set("action_pressed", bind(L, action(1)));
    t.set("action_released", bind(L, action(2)));
    t.set("held_time", bind(L, [ctx](const std::string& name) -> Result<float> {
              auto s = action_state(*ctx, name);
              if (!s.value) return std::unexpected(s.value.error());
              return (*s.value)->actuated ? (*s.value)->held_s : 0.0F;
          }));
    // Contexts apply from the next frame's evaluation.
    t.set("push_context", bind(L, [ctx](const std::string& name) -> Result<void> {
              if (ctx->action_service == nullptr) return fail("no input actions in this session");
              if (auto ok = ctx->action_service->push_context(name); !ok) return fail(ok.error().message);
              return {};
          }));
    t.set("pop_context", bind(L, [ctx](const std::string& name) -> Result<bool> {
              if (ctx->action_service == nullptr) return fail("no input actions in this session");
              return ctx->action_service->pop_context(name);
          }));
}

} // namespace

void register_input(LuaVm& vm, ApiContext& ctx) {
    lua_State* L = vm.state();
    sol::table t(L, sol::create);
    bind_keys(t, L, &ctx);
    bind_gamepad(t, L, &ctx);
    bind_virtual(t, L, &ctx);
    vm.expose("input", t);
}

} // namespace engine::script::detail
