// terrain.material codec (R26.1): the layered terrain material as JSON.
//
//   "material": "alpine"                     a preset by name, or
//   "material": {"preset": "alpine",         start from a preset (optional),
//                "blend_depth": 0.15, ...,   then override global fields
//                "layers": [{...}, ...]}     and the layer stack
//
// `layers` replaces the stack: the result has one layer per entry, and each
// entry is merged over the layer that was at its index (fields an entry
// omits keep that layer's values; new layers start from the defaults).

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "component_json_internal.hpp"
#include "engine/terrain/material.hpp"

namespace engine::scene::detail {

namespace {

using json = nlohmann::json;
using core::EnumName;
using core::JsonFields;
using terrain::AltitudeRef;
using terrain::MaterialLayer;
using terrain::PaintChannel;
using terrain::TerrainMaterial;

constexpr EnumName<AltitudeRef> k_altitude_refs[] = {{"world", AltitudeRef::world},
                                                     {"snowline", AltitudeRef::snowline}};
constexpr EnumName<PaintChannel> k_paint_channels[] = {
    {"none", PaintChannel::none}, {"r", PaintChannel::r}, {"g", PaintChannel::g},
    {"b", PaintChannel::b}};

constexpr float k_altitude_limit = 100000.0F;

[[nodiscard]] std::string preset_list() {
    std::string s;
    for (const std::string_view n : terrain::material_preset_names) {
        s += (s.empty() ? "\"" : ", \"") + std::string(n) + "\"";
    }
    return s;
}

[[nodiscard]] std::string texture_set_list() {
    std::string s;
    for (const std::string_view n : terrain::texture_set_names) {
        s += (s.empty() ? "\"" : ", \"") + std::string(n) + "\"";
    }
    return s;
}

template <typename E, std::size_t N>
[[nodiscard]] std::string_view name_of(const EnumName<E> (&names)[N], E v) {
    for (const auto& n : names) {
        if (n.value == v) return n.name;
    }
    return names[0].name;
}

[[nodiscard]] json encode_layer(const MaterialLayer& l) {
    const terrain::LayerRule& r = l.rule;
    return {{"name", l.name},
            {"texture", l.texture_set},
            {"tint", json::array({l.tint.r, l.tint.g, l.tint.b})},
            {"uv_scale", l.uv_scale_m},
            {"normal_strength", l.normal_strength},
            {"roughness", l.roughness},
            {"height_offset", l.height_offset},
            {"slope", json::array({r.slope_deg.x, r.slope_deg.y})},
            {"slope_softness", r.slope_softness_deg},
            {"altitude", json::array({r.altitude_m.x, r.altitude_m.y})},
            {"altitude_softness", r.altitude_softness_m},
            {"altitude_ref", name_of(k_altitude_refs, r.altitude_ref)},
            {"concavity", r.concavity},
            {"flow", r.flow},
            {"noise", r.noise},
            {"strength", r.strength},
            {"paint", name_of(k_paint_channels, r.paint)}};
}

// Merges one `layers` entry over `l`; returns the entry's error, if any.
[[nodiscard]] std::expected<void, core::Error> decode_layer(const json& v, MaterialLayer& l,
                                                            bool strict) {
    JsonFields f(v, "", strict);
    f.string("name", l.name, 64);
    std::string set = l.texture_set;
    f.string("texture", set, 64);
    if (set != l.texture_set) {
        if (terrain::is_texture_set(set)) {
            l.texture_set = set;
        } else {
            f.fail("texture", "expected one of " + texture_set_list());
        }
    }
    f.color3("tint", l.tint, 4.0F);
    f.number("uv_scale", l.uv_scale_m, 0.05F, 1000.0F);
    f.number("normal_strength", l.normal_strength, 0.0F, 2.0F);
    f.number("roughness", l.roughness, 0.0F, 2.0F);
    f.number("height_offset", l.height_offset, -1.0F, 1.0F);
    terrain::LayerRule& r = l.rule;
    read_range(f, "slope", r.slope_deg, 0.0F, 90.0F);
    f.number("slope_softness", r.slope_softness_deg, 0.0F, 45.0F);
    read_range(f, "altitude", r.altitude_m, -k_altitude_limit, k_altitude_limit);
    f.number("altitude_softness", r.altitude_softness_m, 0.0F, 10000.0F);
    f.enumeration("altitude_ref", r.altitude_ref, k_altitude_refs);
    f.number("concavity", r.concavity, -1.0F, 1.0F);
    f.number("flow", r.flow, -1.0F, 1.0F);
    f.number("noise", r.noise, 0.0F, 1.0F);
    f.number("strength", r.strength, 0.0F, 1.0F);
    f.enumeration("paint", r.paint, k_paint_channels);
    return f.finish();
}

} // namespace

json encode_terrain_material(const TerrainMaterial& m) {
    json layers = json::array();
    for (const MaterialLayer& l : m.layers) layers.push_back(encode_layer(l));
    return {{"preset", m.preset},
            {"blend_depth", m.blend_depth},
            {"triplanar_slope", m.triplanar_slope_deg},
            {"triplanar_sharpness", m.triplanar_sharpness},
            {"stochastic", m.stochastic_tiling},
            {"macro_strength", m.macro_strength},
            {"macro_scale", m.macro_scale_m},
            {"far_start", m.far_start_m},
            {"far_range", m.far_range_m},
            {"layers", std::move(layers)}};
}

void decode_terrain_material(JsonFields& parent, std::string_view key, TerrainMaterial& out) {
    const json* v = parent.raw(key);
    if (v == nullptr || v->is_null()) return;
    const bool strict = parent.strict();
    const auto preset_named = [&](const std::string& name) -> bool {
        if (auto p = terrain::material_preset(name)) {
            out = std::move(*p);
            return true;
        }
        parent.fail(key, "unknown preset \"" + name + "\" (expected one of " + preset_list() + ")");
        return false;
    };
    if (v->is_string()) {
        (void)preset_named(v->get<std::string>());
        return;
    }
    TerrainMaterial m = out;
    JsonFields f(*v, "", strict);
    if (const json* p = f.raw("preset"); p != nullptr && !p->is_null()) {
        if (!p->is_string()) {
            parent.fail(key, "preset: expected a preset name (" + preset_list() + ")");
            return;
        }
        if (auto preset = terrain::material_preset(p->get<std::string>())) {
            m = std::move(*preset);
        } else if (!p->get<std::string>().empty()) {
            parent.fail(key, "preset: unknown preset \"" + p->get<std::string>() +
                                 "\" (expected one of " + preset_list() + ")");
            return;
        }
    }
    f.number("blend_depth", m.blend_depth, 0.01F, 1.0F);
    f.number("triplanar_slope", m.triplanar_slope_deg, 0.0F, 90.0F);
    f.number("triplanar_sharpness", m.triplanar_sharpness, 1.0F, 16.0F);
    f.boolean("stochastic", m.stochastic_tiling);
    f.number("macro_strength", m.macro_strength, 0.0F, 1.0F);
    f.number("macro_scale", m.macro_scale_m, 1.0F, 10000.0F);
    f.number("far_start", m.far_start_m, 0.0F, 100000.0F);
    f.number("far_range", m.far_range_m, 1.0F, 100000.0F);
    if (const json* layers = f.raw("layers"); layers != nullptr && !layers->is_null()) {
        if (!layers->is_array() || layers->empty() ||
            layers->size() > terrain::max_material_layers) {
            parent.fail(key, "layers: expected an array of 1.." +
                                 std::to_string(terrain::max_material_layers) + " layers");
            return;
        }
        std::vector<MaterialLayer> next(layers->size());
        for (std::size_t i = 0; i < layers->size(); ++i) {
            if (i < m.layers.size()) {
                next[i] = m.layers[i];
            } else {
                next[i].texture_set = std::string(terrain::texture_set_names[0]);
            }
            if (auto ok = decode_layer((*layers)[i], next[i], strict); !ok) {
                parent.fail(key, "layers[" + std::to_string(i) + "]." + ok.error().message);
                return;
            }
        }
        m.layers = std::move(next);
    }
    if (auto ok = f.finish(); !ok) {
        parent.fail(key, ok.error().message);
        return;
    }
    out = std::move(m);
}

} // namespace engine::scene::detail
