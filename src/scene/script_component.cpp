// Script component rules and codec — see include/engine/scene/script_component.hpp.

#include "engine/scene/script_component.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

using json = nlohmann::json;

[[nodiscard]] std::unexpected<core::Error> fail(std::string message) {
    return std::unexpected(core::Error{std::move(message)});
}

[[nodiscard]] bool segment_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
}

} // namespace

std::expected<void, core::Error> validate_script_path(std::string_view path) {
    if (path.empty()) return fail("expected a script path relative to assets/scripts/, e.g. \"coin.lua\"");
    if (path.size() > k_max_script_path) {
        return fail("script path longer than " + std::to_string(k_max_script_path) + " bytes");
    }
    if (path.size() < 5 || path.substr(path.size() - 4) != ".lua") {
        return fail("script paths end in \".lua\"");
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view seg = path.substr(start, end - start);
        if (seg.empty()) return fail("empty path segment (no leading, trailing or double '/')");
        if (seg == "." || seg == "..") return fail("'.' and '..' segments are not allowed");
        if (!std::ranges::all_of(seg, segment_char)) {
            return fail("script paths use only letters, digits, '_', '-', '.' and '/' "
                        "(relative to assets/scripts/)");
        }
        start = end + 1;
    }
    return {};
}

std::expected<void, core::Error> validate_property_name(std::string_view name) {
    const auto alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto alnum = [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); };
    if (name.empty() || name.size() > k_max_property_name || !alpha(name.front()) ||
        !std::ranges::all_of(name, alnum)) {
        return fail("property names are identifiers ([A-Za-z_][A-Za-z0-9_]*, at most " +
                    std::to_string(k_max_property_name) + " bytes)");
    }
    if (name == "entity") return fail("'entity' is reserved (self.entity is the script's entity)");
    return {};
}

json property_value_json(const ScriptProperty& p) {
    switch (p.type) {
    case ScriptValueType::number: return p.number;
    case ScriptValueType::boolean: return p.boolean;
    case ScriptValueType::string: return p.text;
    case ScriptValueType::vec3: return json::array({p.vec.x, p.vec.y, p.vec.z});
    }
    return nullptr;
}

std::expected<void, core::Error> decode_property_value(const json& v, ScriptProperty& out) {
    if (v.is_boolean()) {
        out.type = ScriptValueType::boolean;
        out.boolean = v.get<bool>();
        return {};
    }
    if (v.is_number()) {
        const double d = v.get<double>();
        if (!std::isfinite(d)) return fail("expected a finite number");
        out.type = ScriptValueType::number;
        out.number = d;
        return {};
    }
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        if (s.size() > k_max_property_text || s.find('\0') != std::string::npos) {
            return fail("expected a string of at most " + std::to_string(k_max_property_text) +
                        " bytes without NUL");
        }
        out.type = ScriptValueType::string;
        out.text = s;
        return {};
    }
    glm::vec3 vec{};
    if (core::JsonFields::read_vec3(v, vec)) {
        out.type = ScriptValueType::vec3;
        out.vec = vec;
        return {};
    }
    return fail("expected a number, true/false, a string or [x, y, z]");
}

json encode_script(const Script& s) {
    json props = json::object();
    for (const ScriptProperty& p : s.properties) props[p.name] = property_value_json(p);
    return {{"path", s.path}, {"enabled", s.enabled}, {"properties", std::move(props)}};
}

void decode_script_fields(core::JsonFields& f, Script& s) {
    if (f.has("path")) {
        std::string path;
        f.string("path", path, k_max_script_path);
        if (auto ok = validate_script_path(path); f.ok() && !ok) {
            f.fail("path", ok.error().message);
        } else if (f.ok()) {
            s.path = std::move(path);
        }
    }
    f.boolean("enabled", s.enabled);
    const json* props = f.raw("properties");
    if (props == nullptr || props->is_null()) return;
    if (!props->is_object()) {
        f.fail("properties", "expected an object {name: number | true/false | string | [x, y, z]}");
        return;
    }
    std::vector<ScriptProperty> merged = s.properties;
    for (const auto& [name, value] : props->items()) {
        if (auto ok = validate_property_name(name); !ok) {
            f.fail("properties." + name, ok.error().message);
            return;
        }
        const auto it = std::ranges::find(merged, name, &ScriptProperty::name);
        if (value.is_null()) { // removes an override
            if (it != merged.end()) merged.erase(it);
            continue;
        }
        ScriptProperty p;
        if (it != merged.end()) {
            p = *it;
        } else {
            p.name = name;
        }
        if (auto ok = decode_property_value(value, p); !ok) {
            f.fail("properties." + name, ok.error().message);
            return;
        }
        if (it != merged.end()) {
            *it = std::move(p);
        } else {
            merged.push_back(std::move(p));
        }
    }
    if (merged.size() > k_max_script_properties) {
        f.fail("properties", "at most " + std::to_string(k_max_script_properties) + " properties");
        return;
    }
    s.properties = std::move(merged);
    ++s.revision;
}

} // namespace engine::scene
