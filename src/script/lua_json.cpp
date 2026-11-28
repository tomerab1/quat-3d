// JSON <-> Lua conversion for the component codec — see script_api.hpp.

#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "script_api.hpp"

namespace engine::script::detail {

namespace {

using json = nlohmann::json;

constexpr int         k_max_depth = 16;
constexpr std::size_t k_max_elements = 100'000;

void push_json(lua_State* L, const json& v, int depth) {
    switch (v.type()) {
    case json::value_t::null: lua_pushnil(L); return;
    case json::value_t::boolean: lua_pushboolean(L, v.get<bool>() ? 1 : 0); return;
    case json::value_t::number_integer: lua_pushinteger(L, v.get<std::int64_t>()); return;
    case json::value_t::number_unsigned:
        lua_pushinteger(L, static_cast<lua_Integer>(v.get<std::uint64_t>()));
        return;
    case json::value_t::number_float: lua_pushnumber(L, v.get<double>()); return;
    case json::value_t::string: {
        const auto& s = v.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
        return;
    }
    case json::value_t::array: {
        if (v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) {
            sol::stack::push(L, glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()));
            return;
        }
        lua_createtable(L, static_cast<int>(std::min<std::size_t>(v.size(), 1U << 20)), 0);
        if (depth >= k_max_depth) return;
        lua_Integer i = 1;
        for (const json& item : v) {
            push_json(L, item, depth + 1);
            lua_rawseti(L, -2, i++);
        }
        return;
    }
    case json::value_t::object: {
        lua_createtable(L, 0, static_cast<int>(std::min<std::size_t>(v.size(), 1U << 20)));
        if (depth >= k_max_depth) return;
        for (const auto& [key, item] : v.items()) {
            push_json(L, item, depth + 1);
            lua_setfield(L, -2, key.c_str());
        }
        return;
    }
    default: lua_pushnil(L); return;
    }
}

struct Converter {
    std::size_t elements = 0;

    Result<json> convert(const sol::object& v, int depth, const std::string& where) {
        if (++elements > k_max_elements) return fail(where + ": too many values");
        switch (v.get_type()) {
        case sol::type::lua_nil: return json(nullptr);
        case sol::type::boolean: return json(v.as<bool>());
        case sol::type::number: {
            lua_State* L = v.lua_state();
            v.push(L);
            const bool is_int = lua_isinteger(L, -1) != 0;
            const lua_Integer i = lua_tointeger(L, -1);
            const lua_Number n = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (is_int) return json(static_cast<std::int64_t>(i));
            if (!std::isfinite(n)) return fail(where + ": numbers must be finite");
            return json(n);
        }
        case sol::type::string: return json(v.as<std::string>());
        case sol::type::userdata: {
            if (v.is<glm::vec3>()) {
                const glm::vec3 p = v.as<glm::vec3>();
                return json::array({p.x, p.y, p.z});
            }
            if (v.is<glm::quat>()) {
                const glm::quat q = v.as<glm::quat>();
                return json::array({q.x, q.y, q.z, q.w});
            }
            if (v.is<ScriptEntity>()) return json(entity_id(v.as<ScriptEntity>().id));
            return fail(where + ": cannot convert this userdata to JSON");
        }
        case sol::type::table: return convert_table(v.as<sol::table>(), depth, where, false);
        default: return fail(where + ": cannot convert a " + sol::type_name(v.lua_state(), v.get_type()) + " to JSON");
        }
    }

    Result<json> convert_table(const sol::table& t, int depth, const std::string& where,
                               bool force_object) {
        if (depth >= k_max_depth) return fail(where + ": nested too deeply");
        // Raw access only: a script's metamethods must not run inside the engine.
        t.push();
        const std::size_t n = lua_rawlen(t.lua_state(), -1);
        lua_pop(t.lua_state(), 1);
        std::size_t count = 0;
        bool string_keys = false;
        bool other_keys = false;
        for (const auto& [key, value] : t) {
            (void)value;
            ++count;
            if (key.get_type() == sol::type::string) {
                string_keys = true;
            } else if (key.get_type() != sol::type::number) {
                other_keys = true;
            }
            if (count > k_max_elements) return fail(where + ": too many values");
        }
        if (other_keys) return fail(where + ": table keys must be strings (objects) or 1..n (arrays)");
        const bool array = !force_object && !string_keys && count == n;
        if (array) {
            json out = json::array();
            for (std::size_t i = 1; i <= n; ++i) {
                auto item = convert(t.raw_get<sol::object>(i), depth + 1, where + "[" + std::to_string(i) + "]");
                if (!item.value) return std::unexpected(item.value.error());
                out.push_back(std::move(*item.value));
            }
            return out;
        }
        if (!string_keys && count > 0) return fail(where + ": mixes array and object keys");
        json out = json::object();
        for (const auto& [key, value] : t) {
            if (key.get_type() != sol::type::string) {
                return fail(where + ": mixes array and object keys");
            }
            const std::string k = key.as<std::string>();
            auto item = convert(value, depth + 1, where.empty() ? k : where + "." + k);
            if (!item.value) return std::unexpected(item.value.error());
            out[k] = std::move(*item.value);
        }
        return out;
    }
};

} // namespace

sol::object json_to_lua(lua_State* L, const nlohmann::json& value) {
    push_json(L, value, 0);
    sol::object out(L, -1);
    lua_pop(L, 1);
    return out;
}

Result<nlohmann::json> lua_to_json(const sol::object& value, bool top_object) {
    Converter c;
    if (top_object) {
        if (value.get_type() != sol::type::table) {
            return fail("expected a table of fields, e.g. {radius = 2}");
        }
        return c.convert_table(value.as<sol::table>(), 0, "", true);
    }
    return c.convert(value, 0, "value");
}

} // namespace engine::script::detail
