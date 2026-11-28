// WeatherState JSON codec — see include/engine/scene/weather_json.hpp.

#include "engine/scene/weather_json.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/weather/weather_state.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;
using weather::WeatherFieldDesc;
using weather::WeatherFieldRef;
using weather::WeatherFieldType;
using weather::WeatherState;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::string trim(float f) {
    std::string s = std::to_string(f);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

[[nodiscard]] json value_json(const WeatherFieldDesc& d, WeatherState& s) {
    const WeatherFieldRef r = d.ref(s);
    if (r.b != nullptr) return *r.b;
    if (r.p != nullptr) return std::string(weather::precipitation_type_name(*r.p));
    return *r.f;
}

[[nodiscard]] json default_json(const WeatherFieldDesc& d) {
    WeatherState defaults;
    return value_json(d, defaults);
}

[[nodiscard]] std::string expectation(const WeatherFieldDesc& d) {
    switch (d.type) {
    case WeatherFieldType::boolean: return "expected true or false";
    case WeatherFieldType::precipitation_type: return "expected one of \"rain\", \"snow\"";
    case WeatherFieldType::number: break;
    }
    std::string s = "expected a number in [" + trim(d.min) + ", " + trim(d.max) + "]";
    if (!d.unit.empty()) s += " (" + std::string(d.unit) + ")";
    return s;
}

[[nodiscard]] std::string known_fields() {
    std::string s;
    for (const WeatherFieldDesc& d : weather::weather_fields()) {
        s += (s.empty() ? "" : ", ") + std::string(d.name);
    }
    return s;
}

// Decodes one value into `out`; false when it does not fit.
[[nodiscard]] bool decode_value(const WeatherFieldDesc& d, const json& v, bool strict,
                                WeatherState& out) {
    const WeatherFieldRef r = d.ref(out);
    if (r.b != nullptr) {
        if (!v.is_boolean()) return false;
        *r.b = v.get<bool>();
        return true;
    }
    if (r.p != nullptr) {
        if (!v.is_string()) return false;
        return weather::parse_precipitation_type(v.get_ref<const std::string&>(), *r.p);
    }
    float f = 0.0F;
    if (!core::JsonFields::read_float(v, f)) return false;
    if (f < d.min || f > d.max) {
        if (strict) return false;
        f = std::clamp(f, d.min, d.max);
    }
    *r.f = f;
    return true;
}

} // namespace

json encode_weather(const WeatherState& state) {
    WeatherState s = state;
    json out = json::object();
    for (const WeatherFieldDesc& d : weather::weather_fields()) {
        out[std::string(d.name)] = value_json(d, s);
    }
    return out;
}

std::expected<void, core::Error> apply_weather_json(WeatherState& state, const json& fields,
                                                    bool strict, std::string_view context) {
    const std::string where(context);
    if (!fields.is_object()) return fail(where + ": expected an object of weather fields");
    WeatherState scratch = state;
    for (const auto& [key, value] : fields.items()) {
        const WeatherFieldDesc* d = weather::find_weather_field(key);
        if (d == nullptr) {
            if (!strict) continue;
            return fail(where + "." + key + ": unknown field (known: " + known_fields() + ")");
        }
        if (value.is_null()) continue; // explicit null == absent, like JsonFields
        if (!decode_value(*d, value, strict, scratch)) {
            return fail(where + "." + key + ": " + expectation(*d));
        }
    }
    state = scratch;
    return {};
}

json weather_schema() {
    json fields = json::array();
    for (const WeatherFieldDesc& d : weather::weather_fields()) {
        json f = {{"name", d.name},
                  {"group", d.group},
                  {"label", d.label},
                  {"default", default_json(d)},
                  {"hint", d.hint}};
        switch (d.type) {
        case WeatherFieldType::number:
            f["type"] = "number";
            f["min"] = d.min;
            f["max"] = d.max;
            break;
        case WeatherFieldType::boolean: f["type"] = "boolean"; break;
        case WeatherFieldType::precipitation_type:
            f["type"] = "enum";
            f["values"] = json::array({"rain", "snow"});
            break;
        }
        if (!d.unit.empty()) f["unit"] = d.unit;
        fields.push_back(std::move(f));
    }
    return {{"fields", std::move(fields)}};
}

} // namespace engine::scene
