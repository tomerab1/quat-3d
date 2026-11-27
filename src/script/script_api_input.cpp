// The `input` table scripts use — see docs/scripting.md ("Input").

#include <string>
#include <tuple>

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

void bind_virtual(sol::table& t, lua_State* L, ApiContext* ctx) {
    t.set("axis", bind(L, [ctx](const std::string& name) -> Result<float> {
              const ScriptInput& in = input_of(*ctx);
              const auto v = input_axis(in, name);
              if (!v) return fail("unknown axis '" + name + "' (move_x, move_y, look_x, look_y)");
              return in.keyboard_blocked && name.starts_with("move") ? 0.0F : *v;
          }));
    t.set("move_vector", bind(L, [ctx]() -> Result<glm::vec3> {
              const ScriptInput& in = input_of(*ctx);
              if (in.keyboard_blocked) return glm::vec3(0.0F);
              glm::vec3 v(*input_axis(in, "move_x"), 0.0F, -*input_axis(in, "move_y"));
              if (glm::length(v) > 1.0F) v = glm::normalize(v);
              return v;
          }));
    const auto action = [ctx](ActionPhase phase) {
        return [ctx, phase](const std::string& name) -> Result<bool> {
            const ScriptInput& in = input_of(*ctx);
            const auto v = input_action(in, name, phase);
            if (!v) return fail("unknown action '" + name + "' (jump, fire, aim, interact, sprint, pause)");
            return !in.keyboard_blocked && *v;
        };
    };
    t.set("action", bind(L, action(ActionPhase::held)));
    t.set("action_pressed", bind(L, action(ActionPhase::pressed)));
    t.set("action_released", bind(L, action(ActionPhase::released)));
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
