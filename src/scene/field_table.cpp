// Field tables — see include/engine/scene/field_table.hpp.

#include "engine/scene/field_table.hpp"

#include <cstdint>
#include <string>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_json.hpp"

namespace engine::scene {

namespace {

using json = nlohmann::json;

template <typename V>
[[nodiscard]] json vec_json(const V& v) {
    json out = json::array();
    for (glm::length_t i = 0; i < V::length(); ++i) out.push_back(v[i]);
    return out;
}

template <typename V>
[[nodiscard]] bool in_range(const V& v, const FieldDesc& d) {
    for (glm::length_t i = 0; i < V::length(); ++i) {
        const auto c = static_cast<double>(v[i]);
        if (c < d.min || c > d.max) return false;
    }
    return true;
}

[[nodiscard]] std::string range_text(const FieldDesc& d) {
    return "[" + json(d.min).dump() + ", " + json(d.max).dump() + "]";
}

template <typename V>
void read_vec(core::JsonFields& f, const FieldDesc& d, const json& v, V& out) {
    V r{};
    bool ok = v.is_array() && v.size() == static_cast<std::size_t>(V::length());
    for (glm::length_t i = 0; ok && i < V::length(); ++i) {
        ok = core::JsonFields::read_float(v[static_cast<std::size_t>(i)], r[i]);
    }
    if (!ok || !in_range(r, d)) {
        f.fail(d.name, "expected " + std::to_string(V::length()) + " numbers in " + range_text(d));
        return;
    }
    out = r;
}

template <typename V>
void read_color(core::JsonFields& f, const FieldDesc& d, const json& v, V& out) {
    glm::vec4 c(0.0F, 0.0F, 0.0F, 1.0F);
    for (glm::length_t i = 0; i < V::length(); ++i) c[i] = out[i];
    if (auto ok = decode_color(v, c); !ok) {
        f.fail(d.name, ok.error().message);
        return;
    }
    for (glm::length_t i = 0; i < V::length(); ++i) out[i] = c[i];
}

} // namespace

json encode_field_table(const FieldTable& table, const void* component) {
    json out = json::object();
    for (std::size_t i = 0; i < table.fields.size(); ++i) {
        const FieldDesc& d = table.fields[i];
        // The accessors are shared with decoding (non-const); encoding only reads.
        const void* p = table.address[i](const_cast<void*>(component));
        switch (d.kind) {
        case FieldKind::boolean: out[d.name] = *static_cast<const bool*>(p); break;
        case FieldKind::integer: out[d.name] = *static_cast<const std::int32_t*>(p); break;
        case FieldKind::number: out[d.name] = *static_cast<const float*>(p); break;
        case FieldKind::vec2: out[d.name] = vec_json(*static_cast<const glm::vec2*>(p)); break;
        case FieldKind::vec3:
        case FieldKind::color3: out[d.name] = vec_json(*static_cast<const glm::vec3*>(p)); break;
        case FieldKind::color4: out[d.name] = vec_json(*static_cast<const glm::vec4*>(p)); break;
        case FieldKind::string: out[d.name] = *static_cast<const std::string*>(p); break;
        case FieldKind::choice: {
            const std::int32_t index = *static_cast<const std::int32_t*>(p);
            out[d.name] = index >= 0 && static_cast<std::size_t>(index) < d.choices.size()
                              ? json(d.choices[static_cast<std::size_t>(index)])
                              : json(nullptr);
            break;
        }
        }
    }
    return out;
}

void decode_field_table(core::JsonFields& f, const FieldTable& table, void* component) {
    for (std::size_t i = 0; i < table.fields.size(); ++i) {
        const FieldDesc& d = table.fields[i];
        void* p = table.address[i](component);
        if (d.read_only) {
            f.known({d.name});
            (void)f.raw(d.name); // echoed back by encode; ignored on input
            continue;
        }
        switch (d.kind) {
        case FieldKind::boolean: f.boolean(d.name, *static_cast<bool*>(p)); continue;
        case FieldKind::integer: {
            const auto lo = static_cast<std::int64_t>(std::max(d.min, -2147483648.0));
            const auto hi = static_cast<std::int64_t>(std::min(d.max, 2147483647.0));
            f.integer(d.name, *static_cast<std::int32_t*>(p), lo, hi);
            continue;
        }
        case FieldKind::number:
            f.number(d.name, *static_cast<float*>(p), static_cast<float>(std::max(d.min, -3.0e38)),
                     static_cast<float>(std::min(d.max, 3.0e38)));
            continue;
        case FieldKind::string: f.string(d.name, *static_cast<std::string*>(p), d.max_length); continue;
        default: break;
        }
        f.known({d.name});
        const json* v = f.raw(d.name);
        if (v == nullptr || v->is_null()) continue;
        switch (d.kind) {
        case FieldKind::vec2: read_vec(f, d, *v, *static_cast<glm::vec2*>(p)); break;
        case FieldKind::vec3: read_vec(f, d, *v, *static_cast<glm::vec3*>(p)); break;
        case FieldKind::color3: read_color(f, d, *v, *static_cast<glm::vec3*>(p)); break;
        case FieldKind::color4: read_color(f, d, *v, *static_cast<glm::vec4*>(p)); break;
        case FieldKind::choice: {
            bool found = false;
            for (std::size_t c = 0; c < d.choices.size() && v->is_string(); ++c) {
                if (d.choices[c] == v->get_ref<const std::string&>()) {
                    *static_cast<std::int32_t*>(p) = static_cast<std::int32_t>(c);
                    found = true;
                }
            }
            if (!found) {
                std::string names;
                for (const std::string& c : d.choices) names += (names.empty() ? "\"" : ", \"") + c + "\"";
                f.fail(d.name, "expected one of " + names);
            }
            break;
        }
        default: break;
        }
    }
}

json field_table_schema(const FieldTable& table, std::string_view title) {
    json properties = json::object();
    for (const FieldDesc& d : table.fields) {
        json p = json::object();
        const auto numeric = [&](json& s, const char* type) {
            s["type"] = type;
            if (d.min > -1.0e30) s["minimum"] = d.min;
            if (d.max < 1.0e30) s["maximum"] = d.max;
        };
        const auto array_of = [&](int n) {
            json item = json::object();
            numeric(item, "number");
            p = {{"type", "array"}, {"items", item}, {"minItems", n}, {"maxItems", n}};
        };
        switch (d.kind) {
        case FieldKind::boolean: p["type"] = "boolean"; break;
        case FieldKind::integer: numeric(p, "integer"); break;
        case FieldKind::number: numeric(p, "number"); break;
        case FieldKind::vec2: array_of(2); break;
        case FieldKind::vec3: array_of(3); break;
        case FieldKind::color3:
        case FieldKind::color4: {
            const int n = d.kind == FieldKind::color3 ? 3 : 4;
            json hex = {{"type", "string"}, {"minLength", 7}, {"maxLength", 9},
                        {"description", "\"#rrggbb\" or \"#rrggbbaa\" (sRGB)"}};
            json arr = {{"type", "array"},
                        {"items", {{"type", "number"}, {"minimum", 0}}},
                        {"minItems", 3},
                        {"maxItems", n}};
            p = {{"anyOf", json::array({arr, hex})}};
            break;
        }
        case FieldKind::string: p = {{"type", "string"}, {"maxLength", d.max_length}}; break;
        case FieldKind::choice: p = {{"enum", d.choices}}; break;
        }
        if (!d.description.empty()) p["description"] = d.description;
        if (d.read_only) p["readOnly"] = true;
        properties[d.name] = std::move(p);
    }
    return {{"$schema", "https://json-schema.org/draft/2020-12/schema"},
            {"title", std::string(title)},
            {"type", "object"},
            {"properties", std::move(properties)},
            {"additionalProperties", false}};
}

} // namespace engine::scene
