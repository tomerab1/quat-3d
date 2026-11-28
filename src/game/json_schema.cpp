// JSON Schema subset — see include/engine/game/json_schema.hpp. Follows the
// JSON Schema 2020-12 validation vocabulary (json-schema.org/draft/2020-12,
// "JSON Schema Validation"), restricted to the keywords listed there.

#include "engine/game/json_schema.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <string_view>

#include <nlohmann/json.hpp>

namespace engine::game {

namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 24> k_keywords = {
    "$schema", "title", "description", "default", "examples", "readOnly",
    "type", "properties", "required", "additionalProperties", "items", "minItems",
    "maxItems", "enum", "const", "minimum", "maximum", "exclusiveMinimum",
    "exclusiveMaximum", "minLength", "maxLength", "anyOf", "$comment", "deprecated"};
constexpr std::array<std::string_view, 7> k_types = {"null",   "boolean", "object", "array",
                                                     "number", "string",  "integer"};
constexpr std::size_t k_max_depth = 32;

[[nodiscard]] std::unexpected<core::Error> fail(const std::string& path, const std::string& what) {
    return std::unexpected(core::Error{path + ": " + what});
}

[[nodiscard]] bool known(std::string_view key) {
    for (std::string_view k : k_keywords) {
        if (k == key) return true;
    }
    return false;
}

[[nodiscard]] bool type_name(const json& t) {
    if (!t.is_string()) return false;
    for (std::string_view k : k_types) {
        if (k == t.get_ref<const std::string&>()) return true;
    }
    return false;
}

std::expected<void, core::Error> check(const json& s, const std::string& path, std::size_t depth) {
    if (depth > k_max_depth) return fail(path, "schema nested too deeply");
    if (s.is_boolean()) return {};
    if (!s.is_object()) return fail(path, "a schema is an object or a boolean");
    for (const auto& [key, v] : s.items()) {
        const std::string at = path.empty() ? key : path + "." + key;
        if (!known(key)) return fail(at, "unsupported keyword (see engine/game/json_schema.hpp)");
        if (key == "type") {
            bool ok = type_name(v);
            if (v.is_array()) {
                ok = !v.empty();
                for (const json& t : v) ok = ok && type_name(t);
            }
            if (!ok) return fail(at, "expected a type name or an array of type names");
        } else if (key == "properties") {
            if (!v.is_object()) return fail(at, "expected an object of schemas");
            for (const auto& [name, sub] : v.items()) {
                if (auto ok = check(sub, at + "." + name, depth + 1); !ok) return ok;
            }
        } else if (key == "items") {
            if (auto ok = check(v, at, depth + 1); !ok) return ok;
        } else if (key == "anyOf") {
            if (!v.is_array() || v.empty()) return fail(at, "expected a non-empty array of schemas");
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (auto ok = check(v[i], at + "[" + std::to_string(i) + "]", depth + 1); !ok) return ok;
            }
        } else if (key == "required") {
            bool ok = v.is_array();
            for (const json& n : v) ok = ok && n.is_string();
            if (!ok) return fail(at, "expected an array of property names");
        } else if (key == "additionalProperties") {
            if (!v.is_boolean()) return fail(at, "only true or false is supported");
        } else if (key == "enum") {
            if (!v.is_array() || v.empty()) return fail(at, "expected a non-empty array");
        } else if (key == "minimum" || key == "maximum" || key == "exclusiveMinimum" ||
                   key == "exclusiveMaximum") {
            if (!v.is_number()) return fail(at, "expected a number");
        } else if (key == "minItems" || key == "maxItems" || key == "minLength" || key == "maxLength") {
            if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<std::int64_t>() >= 0)) {
                return fail(at, "expected a non-negative integer");
            }
        }
    }
    return {};
}

[[nodiscard]] bool has_type(const json& value, std::string_view t) {
    if (t == "null") return value.is_null();
    if (t == "boolean") return value.is_boolean();
    if (t == "object") return value.is_object();
    if (t == "array") return value.is_array();
    if (t == "string") return value.is_string();
    if (t == "number") return value.is_number();
    if (t == "integer") {
        if (value.is_number_integer()) return true;
        if (!value.is_number_float()) return false;
        const double d = value.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return false;
}

[[nodiscard]] std::size_t code_points(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1U : 0U;
    return n;
}

std::expected<void, core::Error> validate(const json& s, const json& v, const std::string& path) {
    if (s.is_boolean()) {
        if (!s.get<bool>()) return fail(path, "not allowed");
        return {};
    }
    if (const auto t = s.find("type"); t != s.end()) {
        bool ok = false;
        std::string names;
        const auto consider = [&](const json& name) {
            ok = ok || has_type(v, name.get_ref<const std::string&>());
            names += (names.empty() ? "" : " or ") + name.get<std::string>();
        };
        if (t->is_array()) {
            for (const json& name : *t) consider(name);
        } else {
            consider(*t);
        }
        if (!ok) return fail(path, "expected " + names);
    }
    if (const auto e = s.find("enum"); e != s.end()) {
        bool found = false;
        for (const json& option : *e) found = found || option == v;
        if (!found) return fail(path, "expected one of " + e->dump(-1, ' ', false, json::error_handler_t::replace));
    }
    if (const auto c = s.find("const"); c != s.end() && *c != v) {
        return fail(path, "expected " + c->dump(-1, ' ', false, json::error_handler_t::replace));
    }
    if (v.is_number()) {
        const double d = v.get<double>();
        const auto bound = [&](const char* key, auto ok, const char* what) -> std::expected<void, core::Error> {
            if (const auto b = s.find(key); b != s.end() && !ok(d, b->template get<double>())) {
                return fail(path, std::string("expected a number ") + what + " " + b->dump());
            }
            return {};
        };
        if (auto ok = bound("minimum", [](double x, double b) { return x >= b; }, ">="); !ok) return ok;
        if (auto ok = bound("maximum", [](double x, double b) { return x <= b; }, "<="); !ok) return ok;
        if (auto ok = bound("exclusiveMinimum", [](double x, double b) { return x > b; }, ">"); !ok) return ok;
        if (auto ok = bound("exclusiveMaximum", [](double x, double b) { return x < b; }, "<"); !ok) return ok;
    }
    if (v.is_string()) {
        const std::size_t n = code_points(v.get_ref<const std::string&>());
        if (const auto m = s.find("minLength"); m != s.end() && n < m->get<std::size_t>()) {
            return fail(path, "expected at least " + m->dump() + " characters");
        }
        if (const auto m = s.find("maxLength"); m != s.end() && n > m->get<std::size_t>()) {
            return fail(path, "expected at most " + m->dump() + " characters");
        }
    }
    if (v.is_array()) {
        if (const auto m = s.find("minItems"); m != s.end() && v.size() < m->get<std::size_t>()) {
            return fail(path, "expected at least " + m->dump() + " items");
        }
        if (const auto m = s.find("maxItems"); m != s.end() && v.size() > m->get<std::size_t>()) {
            return fail(path, "expected at most " + m->dump() + " items");
        }
        if (const auto items = s.find("items"); items != s.end()) {
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (auto ok = validate(*items, v[i], path + "[" + std::to_string(i) + "]"); !ok) return ok;
            }
        }
    }
    if (v.is_object()) {
        const auto props = s.find("properties");
        if (const auto req = s.find("required"); req != s.end()) {
            for (const json& name : *req) {
                if (!v.contains(name.get_ref<const std::string&>())) {
                    return fail(path + "." + name.get<std::string>(), "required");
                }
            }
        }
        for (const auto& [key, sub] : v.items()) {
            const std::string at = path + "." + key;
            if (props != s.end() && props->contains(key)) {
                if (auto ok = validate((*props)[key], sub, at); !ok) return ok;
            } else if (const auto extra = s.find("additionalProperties");
                       extra != s.end() && !extra->get<bool>()) {
                std::string names;
                if (props != s.end()) {
                    for (const auto& [n, unused] : props->items()) names += (names.empty() ? "" : ", ") + n;
                }
                return fail(at, "unknown field" + (names.empty() ? std::string() : " (known: " + names + ")"));
            }
        }
    }
    if (const auto any = s.find("anyOf"); any != s.end()) {
        std::string first;
        for (const json& option : *any) {
            auto ok = validate(option, v, path);
            if (ok) return {};
            if (first.empty()) first = ok.error().message;
        }
        return std::unexpected(core::Error{first});
    }
    return {};
}

} // namespace

std::expected<void, core::Error> check_schema(const json& schema) { return check(schema, "", 0); }

std::expected<void, core::Error> validate_json(const json& schema, const json& value,
                                               const std::string& path) {
    return validate(schema, value, path);
}

} // namespace engine::game
