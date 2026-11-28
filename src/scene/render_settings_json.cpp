// RenderSettings JSON codec — see include/engine/scene/render_settings_json.hpp.

#include "engine/scene/render_settings_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/renderer/render_settings.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;
using renderer::RenderSettingDesc;
using renderer::RenderSettingGroup;
using renderer::RenderSettings;
using renderer::RenderSettingType;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::string trim(float f) {
    std::string s = std::to_string(f);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

[[nodiscard]] const char* type_name(RenderSettingType t) {
    switch (t) {
    case RenderSettingType::boolean: return "boolean";
    case RenderSettingType::number: return "number";
    case RenderSettingType::integer: return "integer";
    case RenderSettingType::choice: return "string";
    }
    return "number";
}

[[nodiscard]] json value_json(const RenderSettingDesc& d, float v) {
    switch (d.type) {
    case RenderSettingType::boolean: return v != 0.0F;
    case RenderSettingType::integer: return static_cast<std::int64_t>(std::lround(v));
    case RenderSettingType::number: return v;
    case RenderSettingType::choice: return std::string(renderer::render_setting_choice_name(d, v));
    }
    return v;
}

[[nodiscard]] std::string expectation(const RenderSettingDesc& d) {
    if (d.type == RenderSettingType::boolean) return "expected true or false";
    if (d.type == RenderSettingType::choice) {
        std::string s = "expected one of ";
        for (std::size_t i = 0; i < d.choices.size(); ++i) {
            s += (i == 0 ? "\"" : ", \"") + std::string(d.choices[i]) + "\"";
        }
        return s;
    }
    std::string s = std::string("expected ") +
                    (d.type == RenderSettingType::integer ? "an integer" : "a number") + " in [" +
                    trim(d.min) + ", " + trim(d.max) + "]";
    if (!d.unit.empty()) s += " (" + std::string(d.unit) + ")";
    return s;
}

// Decodes one value; nullopt (with `error` set) when it does not fit.
[[nodiscard]] std::optional<float> decode_value(const RenderSettingDesc& d, const json& v,
                                                bool strict) {
    if (d.type == RenderSettingType::boolean) {
        if (!v.is_boolean()) return std::nullopt;
        return v.get<bool>() ? 1.0F : 0.0F;
    }
    if (d.type == RenderSettingType::choice) {
        std::string name;
        if (!core::JsonFields::read_string(v, name)) return std::nullopt;
        const std::optional<int> index = renderer::render_setting_choice_index(d, name);
        if (!index) return std::nullopt;
        return static_cast<float>(*index);
    }
    float f = 0.0F;
    if (d.type == RenderSettingType::integer && strict) {
        std::int64_t i = 0;
        if (!core::JsonFields::read_int(v, i)) return std::nullopt;
        f = static_cast<float>(i);
    } else if (!core::JsonFields::read_float(v, f)) {
        return std::nullopt;
    }
    if (f < d.min || f > d.max) {
        if (strict) return std::nullopt;
        f = std::clamp(f, d.min, d.max);
    }
    return f;
}

[[nodiscard]] std::string known_groups() {
    std::string s;
    for (const RenderSettingGroup& g : renderer::render_setting_groups()) {
        s += (s.empty() ? "" : ", ") + std::string(g.name);
    }
    return s;
}

[[nodiscard]] std::string known_fields(const RenderSettingGroup& g) {
    std::string s;
    for (const RenderSettingDesc& d : renderer::render_setting_fields()) {
        if (d.group == g.name) s += (s.empty() ? "" : ", ") + std::string(d.name);
    }
    return s;
}

// Applies one group's fields into `out` (a scratch copy).
[[nodiscard]] std::expected<void, core::Error> apply_group(RenderSettings& out,
                                                           const RenderSettingGroup& g,
                                                           const json& fields, bool strict,
                                                           std::string_view context) {
    const std::string where = std::string(context);
    if (!fields.is_object()) return fail(where + ": expected an object of fields");
    for (const auto& [key, value] : fields.items()) {
        const RenderSettingDesc* d = renderer::find_render_setting(g.name, key);
        if (d == nullptr) {
            // Retired keys (older scene files / clients) are accepted and ignored.
            if (!strict || renderer::find_deprecated_render_setting(g.name, key) != nullptr) {
                continue;
            }
            return fail(where + "." + key + ": unknown field (known: " + known_fields(g) + ")");
        }
        if (value.is_null()) continue; // explicit null == absent, like JsonFields
        const std::optional<float> v = decode_value(*d, value, strict);
        if (!v) return fail(where + "." + key + ": " + expectation(*d));
        renderer::set_render_setting(out, *d, *v);
    }
    return {};
}

} // namespace

json encode_render_setting_group(const RenderSettings& settings, const RenderSettingGroup& group) {
    json out = json::object();
    for (const RenderSettingDesc& d : renderer::render_setting_fields()) {
        if (d.group == group.name) {
            out[std::string(d.name)] = value_json(d, renderer::render_setting_value(settings, d));
        }
    }
    return out;
}

json encode_render_settings(const RenderSettings& settings, std::string_view group) {
    json out = json::object();
    for (const RenderSettingGroup& g : renderer::render_setting_groups()) {
        if (group.empty() || g.name == group) {
            out[std::string(g.name)] = encode_render_setting_group(settings, g);
        }
    }
    return out;
}

std::expected<void, core::Error> apply_render_setting_group(RenderSettings& settings,
                                                            const RenderSettingGroup& group,
                                                            const json& fields, bool strict,
                                                            std::string_view context) {
    RenderSettings scratch = settings;
    if (auto ok = apply_group(scratch, group, fields, strict, context); !ok) return ok;
    settings = scratch;
    return {};
}

std::expected<void, core::Error> apply_render_settings_json(RenderSettings& settings,
                                                            const json& patch, bool strict) {
    if (!patch.is_object()) return fail("render settings: expected an object of groups");
    RenderSettings scratch = settings;
    for (const auto& [key, fields] : patch.items()) {
        const RenderSettingGroup* g = renderer::find_render_setting_group(key);
        if (g == nullptr) {
            if (!strict) continue;
            return fail(key + ": unknown group (known: " + known_groups() + ")");
        }
        if (fields.is_null()) continue;
        if (auto ok = apply_group(scratch, *g, fields, strict, key); !ok) return ok;
    }
    settings = scratch;
    return {};
}

json render_settings_schema() {
    json groups = json::array();
    for (const RenderSettingGroup& g : renderer::render_setting_groups()) {
        json fields = json::array();
        for (const RenderSettingDesc& d : renderer::render_setting_fields()) {
            if (d.group != g.name) continue;
            json f = {{"name", d.name},
                      {"label", d.label},
                      {"type", type_name(d.type)},
                      {"default", value_json(d, d.default_value)},
                      {"hint", d.hint}};
            if (d.type == RenderSettingType::choice) {
                json names = json::array();
                for (const std::string_view c : d.choices) names.push_back(c);
                f["enum"] = std::move(names);
            } else if (d.type != RenderSettingType::boolean) {
                f["min"] = value_json(d, d.min);
                f["max"] = value_json(d, d.max);
            }
            if (!d.unit.empty()) f["unit"] = d.unit;
            fields.push_back(std::move(f));
        }
        json group = {
            {"name", g.name}, {"label", g.label}, {"hint", g.hint}, {"fields", std::move(fields)}};
        json deprecated = json::array();
        for (const renderer::DeprecatedRenderSetting& d : renderer::deprecated_render_settings()) {
            if (d.group == g.name) deprecated.push_back({{"name", d.name}, {"note", d.note}});
        }
        if (!deprecated.empty()) group["deprecated"] = std::move(deprecated);
        groups.push_back(std::move(group));
    }
    return {{"groups", std::move(groups)}};
}

} // namespace engine::scene
