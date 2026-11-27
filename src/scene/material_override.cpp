// Material tuning — see include/engine/scene/material_override.hpp.

#include "engine/scene/material_override.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/deferred_release_queue.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/primitives.hpp"
#include "fnv_hash.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;
using K = MaterialFieldKind;

// Bit index of each field (MaterialOverride::mask).
enum Field : std::uint32_t {
    f_base_color,
    f_emissive,
    f_metallic,
    f_roughness,
    f_occlusion_strength,
    f_normal_scale,
    f_alpha_cutoff,
    f_transmission,
    f_ior,
    f_attenuation_color,
    f_attenuation_distance,
    f_thickness,
    f_clearcoat,
    f_clearcoat_roughness,
};

constexpr std::uint32_t bit(Field f) { return 1U << f; }

constexpr std::array k_fields = {
    MaterialFieldInfo{"base_color", "base colour", K::color4, 0.0F, 1.0F,
                      "Linear base colour and alpha", bit(f_base_color), true},
    MaterialFieldInfo{"emissive", "emissive", K::color3, 0.0F, 1000.0F,
                      "Emitted linear radiance (HDR, may exceed 1)", bit(f_emissive), true},
    MaterialFieldInfo{"metallic", "metallic", K::scalar, 0.0F, 1.0F,
                      "0 = dielectric, 1 = metal", bit(f_metallic), true},
    MaterialFieldInfo{"roughness", "roughness", K::scalar, 0.0F, 1.0F,
                      "0 = mirror, 1 = fully diffuse highlight", bit(f_roughness), true},
    MaterialFieldInfo{"occlusion_strength", "occlusion", K::scalar, 0.0F, 1.0F,
                      "Strength of the occlusion texture", bit(f_occlusion_strength), false},
    MaterialFieldInfo{"normal_scale", "normal scale", K::scalar, 0.0F, 4.0F,
                      "Strength of the normal map", bit(f_normal_scale), false},
    MaterialFieldInfo{"alpha_cutoff", "alpha cutoff", K::scalar, 0.0F, 1.0F,
                      "Alpha-mask threshold", bit(f_alpha_cutoff), false},
    MaterialFieldInfo{"transmission", "transmission", K::scalar, 0.0F, 1.0F,
                      "Refractive glass; > 0 renders in the forward glass pass",
                      bit(f_transmission), false},
    MaterialFieldInfo{"ior", "index of refraction", K::scalar, 1.0F, 3.0F,
                      "Index of refraction (glass 1.5, water 1.33)", bit(f_ior), false},
    MaterialFieldInfo{"attenuation_color", "attenuation colour", K::color3, 0.0F, 1.0F,
                      "Colour light reaches after attenuation_distance in the medium",
                      bit(f_attenuation_color), false},
    MaterialFieldInfo{"attenuation_distance", "attenuation distance", K::scalar, 0.0F, 10000.0F,
                      "Distance for the attenuation colour; 0 = clear medium",
                      bit(f_attenuation_distance), false},
    MaterialFieldInfo{"thickness", "thickness", K::scalar, 0.0F, 10000.0F,
                      "Volume thickness (mesh units) for refraction and absorption",
                      bit(f_thickness), false},
    MaterialFieldInfo{"clearcoat", "clearcoat", K::scalar, 0.0F, 1.0F,
                      "Strength of a clear lacquer layer", bit(f_clearcoat), false},
    MaterialFieldInfo{"clearcoat_roughness", "clearcoat roughness", K::scalar, 0.0F, 1.0F,
                      "Roughness of the clear lacquer layer", bit(f_clearcoat_roughness), false},
};

[[nodiscard]] Field field_index(const MaterialFieldInfo& field) {
    std::uint32_t i = 0;
    while (i < 32 && (field.bit >> i) != 1U) ++i;
    return static_cast<Field>(i);
}

[[nodiscard]] std::string trim(float f) {
    std::string s = std::to_string(f);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

[[nodiscard]] std::string range(const MaterialFieldInfo& f) {
    return "[" + trim(f.min) + ", " + trim(f.max) + "]";
}

[[nodiscard]] glm::vec4 clamp_value(const MaterialFieldInfo& f, glm::vec4 v) {
    switch (f.kind) {
    case K::color4: return glm::clamp(v, glm::vec4(0.0F), glm::vec4(1.0F));
    case K::color3: return {glm::clamp(glm::vec3(v), glm::vec3(f.min), glm::vec3(f.max)), 0.0F};
    case K::scalar: return {std::clamp(v.x, f.min, f.max), 0.0F, 0.0F, 0.0F};
    }
    return v;
}

[[nodiscard]] json value_json(const MaterialFieldInfo& f, const glm::vec4& v) {
    switch (f.kind) {
    case K::color4: return json::array({v.x, v.y, v.z, v.w});
    case K::color3: return json::array({v.x, v.y, v.z});
    case K::scalar: return v.x;
    }
    return nullptr;
}

[[nodiscard]] std::uint64_t override_hash(const MaterialOverride& o, asset::AssetId base) {
    Fnv64 h;
    h.u64(base.value);
    h.u32(o.mask);
    for (const MaterialFieldInfo& f : k_fields) {
        if ((o.mask & f.bit) == 0) continue;
        const glm::vec4 v = material_field_value(o.values, f);
        for (int i = 0; i < 4; ++i) h.f32(v[i]);
    }
    return h.nonzero();
}

} // namespace

std::span<const MaterialFieldInfo> material_fields() { return k_fields; }

const MaterialFieldInfo* find_material_field(std::string_view name) {
    for (const MaterialFieldInfo& f : k_fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

glm::vec4 material_field_value(const asset::PbrMaterialParams& p, const MaterialFieldInfo& field) {
    switch (field_index(field)) {
    case f_base_color: return p.base_color_factor;
    case f_emissive: return {glm::vec3(p.emissive_factor), 0.0F};
    case f_metallic: return {p.metallic_factor, 0.0F, 0.0F, 0.0F};
    case f_roughness: return {p.roughness_factor, 0.0F, 0.0F, 0.0F};
    case f_occlusion_strength: return {p.occlusion_strength, 0.0F, 0.0F, 0.0F};
    case f_normal_scale: return {p.normal_scale, 0.0F, 0.0F, 0.0F};
    case f_alpha_cutoff: return {p.alpha_cutoff, 0.0F, 0.0F, 0.0F};
    case f_transmission: return {p.transmission_factor, 0.0F, 0.0F, 0.0F};
    case f_ior: return {p.ior, 0.0F, 0.0F, 0.0F};
    case f_attenuation_color: return {glm::vec3(p.attenuation), 0.0F};
    case f_attenuation_distance: return {p.attenuation.w, 0.0F, 0.0F, 0.0F};
    case f_thickness: return {p.thickness, 0.0F, 0.0F, 0.0F};
    case f_clearcoat: return {p.clearcoat_factor, 0.0F, 0.0F, 0.0F};
    case f_clearcoat_roughness: return {p.clearcoat_roughness, 0.0F, 0.0F, 0.0F};
    }
    return glm::vec4(0.0F);
}

void set_material_field_value(asset::PbrMaterialParams& p, const MaterialFieldInfo& field,
                              const glm::vec4& value) {
    const glm::vec4 v = clamp_value(field, value);
    switch (field_index(field)) {
    case f_base_color: p.base_color_factor = v; break;
    case f_emissive: p.emissive_factor = glm::vec4(glm::vec3(v), p.emissive_factor.w); break;
    case f_metallic: p.metallic_factor = v.x; break;
    case f_roughness: p.roughness_factor = v.x; break;
    case f_occlusion_strength: p.occlusion_strength = v.x; break;
    case f_normal_scale: p.normal_scale = v.x; break;
    case f_alpha_cutoff: p.alpha_cutoff = v.x; break;
    case f_transmission:
        p.transmission_factor = v.x;
        if (v.x > 0.0F) {
            p.flags |= asset::material_transmission;
        } else {
            p.flags &= ~static_cast<std::uint32_t>(asset::material_transmission);
        }
        break;
    case f_ior: p.ior = v.x; break;
    case f_attenuation_color: p.attenuation = glm::vec4(glm::vec3(v), p.attenuation.w); break;
    case f_attenuation_distance: p.attenuation.w = v.x; break;
    case f_thickness: p.thickness = v.x; break;
    case f_clearcoat: p.clearcoat_factor = v.x; break;
    case f_clearcoat_roughness: p.clearcoat_roughness = v.x; break;
    }
}

asset::PbrMaterialParams apply_material_override(const asset::PbrMaterialParams& base,
                                                 const MaterialOverride& o) {
    asset::PbrMaterialParams out = base;
    for (const MaterialFieldInfo& f : k_fields) {
        if ((o.mask & f.bit) != 0) set_material_field_value(out, f, material_field_value(o.values, f));
    }
    return out;
}

std::optional<asset::PbrMaterialParams> base_material_params(const entt::registry& r,
                                                             entt::entity e) {
    if (const auto* p = r.try_get<Primitive>(e)) return primitive_material_params(*p);
    if (const auto* mr = r.try_get<MeshRenderer>(e); mr != nullptr && mr->material.valid()) {
        return mr->material->params;
    }
    return std::nullopt;
}

std::optional<asset::PbrMaterialParams> effective_material_params(const entt::registry& r,
                                                                  entt::entity e) {
    std::optional<asset::PbrMaterialParams> p = base_material_params(r, e);
    if (!p) return p;
    if (const auto* o = r.try_get<MaterialOverride>(e)) *p = apply_material_override(*p, *o);
    return p;
}

void set_entity_material_field(entt::registry& r, entt::entity e, const MaterialFieldInfo& field,
                               const glm::vec4& value) {
    const glm::vec4 v = clamp_value(field, value);
    if (auto* p = r.try_get<Primitive>(e); p != nullptr && field.primitive_owned) {
        switch (field_index(field)) {
        case f_base_color: p->color = v; break;
        case f_emissive: p->emissive = glm::vec3(v); break;
        case f_metallic: p->metallic = v.x; break;
        case f_roughness: p->roughness = v.x; break;
        default: break;
        }
        clear_material_override_field(r, e, field); // the primitive's value shows
        return;
    }
    auto& o = r.get_or_emplace<MaterialOverride>(e);
    set_material_field_value(o.values, field, v);
    o.mask |= field.bit;
}

void clear_material_override_field(entt::registry& r, entt::entity e,
                                   const MaterialFieldInfo& field) {
    auto* o = r.try_get<MaterialOverride>(e);
    if (o == nullptr) return;
    o->mask &= ~field.bit;
    if (o->mask == 0) r.remove<MaterialOverride>(e);
}

json encode_material_override(const MaterialOverride& o) {
    json out = json::object();
    for (const MaterialFieldInfo& f : k_fields) {
        if ((o.mask & f.bit) != 0) out[std::string(f.name)] = value_json(f, material_field_value(o.values, f));
    }
    return out;
}

json encode_material_params(const asset::PbrMaterialParams& params) {
    json out = json::object();
    for (const MaterialFieldInfo& f : k_fields) {
        out[std::string(f.name)] = value_json(f, material_field_value(params, f));
    }
    return out;
}

json material_fields_schema() {
    json out = json::array();
    for (const MaterialFieldInfo& f : k_fields) {
        const char* kind = f.kind == K::color4 ? "color4" : f.kind == K::color3 ? "color3" : "scalar";
        out.push_back({{"name", f.name},
                       {"label", f.label},
                       {"kind", kind},
                       {"min", f.min},
                       {"max", f.max},
                       {"hint", f.hint},
                       {"primitive_field", f.primitive_owned}});
    }
    return out;
}

std::expected<glm::vec4, core::Error> decode_material_field(const json& value,
                                                           const MaterialFieldInfo& f,
                                                           bool strict) {
    // A number within the field's range; lenient decoding clamps instead.
    const auto in_range = [&](float& x) {
        if (x >= f.min && x <= f.max) return true;
        x = std::clamp(x, f.min, f.max);
        return !strict;
    };
    if (f.kind == K::scalar) {
        float x = 0.0F;
        if (!core::JsonFields::read_float(value, x) || !in_range(x)) {
            return std::unexpected(core::Error{"expected a number in " + range(f)});
        }
        return glm::vec4(x, 0.0F, 0.0F, 0.0F);
    }
    const std::string expected =
        f.kind == K::color4
            ? "expected \"#rrggbb\", \"#rrggbbaa\" (sRGB) or linear [r, g, b(, a)] in [0, 1]"
            : "expected \"#rrggbb\" (sRGB) or linear [r, g, b] in " + range(f);
    if (value.is_string()) {
        glm::vec4 c(0.0F, 0.0F, 0.0F, 1.0F);
        const bool has_alpha = value.get_ref<const std::string&>().size() > 7;
        if (!decode_color(value, c) || (f.kind == K::color3 && has_alpha)) {
            return std::unexpected(core::Error{expected});
        }
        if (f.kind == K::color3) c.w = 0.0F;
        return c;
    }
    const std::size_t n = value.is_array() ? value.size() : 0;
    const bool size_ok = f.kind == K::color4 ? (n == 3 || n == 4) : n == 3;
    if (!size_ok) return std::unexpected(core::Error{expected});
    glm::vec4 c(0.0F, 0.0F, 0.0F, f.kind == K::color4 ? 1.0F : 0.0F);
    for (std::size_t i = 0; i < n; ++i) {
        float x = 0.0F;
        if (!core::JsonFields::read_float(value[i], x) || !in_range(x)) {
            return std::unexpected(core::Error{expected});
        }
        c[static_cast<glm::length_t>(i)] = x;
    }
    return c;
}

std::expected<void, core::Error> decode_material_override(const json& value, MaterialOverride& o,
                                                          bool strict) {
    core::JsonFields fields(value, "material", strict);
    std::uint32_t mask = o.mask;
    asset::PbrMaterialParams values = o.values;
    for (const MaterialFieldInfo& f : k_fields) {
        fields.known({f.name});
        const json* v = fields.raw(f.name);
        if (v == nullptr) continue;
        if (v->is_null()) {
            mask &= ~f.bit;
            continue;
        }
        auto decoded = decode_material_field(*v, f, strict);
        if (!decoded) {
            fields.fail(f.name, decoded.error().message);
            continue;
        }
        set_material_field_value(values, f, *decoded);
        mask |= f.bit;
    }
    if (auto ok = fields.finish(); !ok) return std::unexpected(ok.error());
    o.mask = mask;
    o.values = values;
    return {};
}

// ---- MaterialOverrideStore ----------------------------------------------------------

struct MaterialOverrideStore::Entry {
    std::shared_ptr<asset::MaterialAsset> material; // null after a failed upload
    asset::AssetId                        base{};
    std::uint64_t                         hash = 0;
};

struct MaterialOverrideStore::State {
    explicit State(std::uint32_t frames_in_flight) : retired(frames_in_flight) {}
    std::unordered_map<entt::entity, Entry> entries;
    core::DeferredReleaseQueue              retired;
    std::uint64_t                           last_frame = 0;

    // Retires `material` (if any): destroyed once no frame in flight reads it.
    void retire(std::shared_ptr<asset::MaterialAsset> material, std::uint64_t frame) {
        if (!material) return;
        retired.push(frame, [m = std::move(material)]() mutable { m.reset(); });
    }
};

namespace {

// True when the entity has overrides to apply to a material it draws.
[[nodiscard]] bool has_active_override(const entt::registry& r, entt::entity e) {
    if (!r.valid(e)) return false;
    const auto* mr = r.try_get<MeshRenderer>(e);
    const auto* o = r.try_get<MaterialOverride>(e);
    return mr != nullptr && o != nullptr && o->mask != 0 && mr->material.valid();
}

} // namespace

MaterialOverrideStore::MaterialOverrideStore(std::uint32_t frames_in_flight)
    : state_(std::make_unique<State>(frames_in_flight)) {}
MaterialOverrideStore::~MaterialOverrideStore() = default;
MaterialOverrideStore::MaterialOverrideStore(MaterialOverrideStore&&) noexcept = default;
MaterialOverrideStore& MaterialOverrideStore::operator=(MaterialOverrideStore&&) noexcept = default;

const asset::MaterialAsset* MaterialOverrideStore::drawn(const entt::registry& r, entt::entity e,
                                                         const MeshRenderer& renderer) const {
    if (!renderer.material.valid()) return nullptr;
    const asset::MaterialAsset* base = &*renderer.material;
    const auto it = state_->entries.find(e);
    if (it == state_->entries.end() || !it->second.material) return base;
    const auto* o = r.try_get<MaterialOverride>(e);
    if (o == nullptr || o->mask == 0) return base;
    const asset::AssetId base_id = renderer.material.id();
    const Entry& entry = it->second;
    // Stale (edited since the last resolve, or another entity reusing the id):
    // the base until resolve() catches up.
    if (entry.base != base_id || entry.hash != override_hash(*o, base_id)) return base;
    return entry.material.get();
}

bool MaterialOverrideStore::needs_resolve(const entt::registry& r) const {
    for (const auto& [e, entry] : state_->entries) {
        if (!has_active_override(r, e)) return true; // orphaned
    }
    for (auto [e, mr, o] : r.view<const MeshRenderer, const MaterialOverride>().each()) {
        if (o.mask == 0 || !mr.material.valid()) continue;
        const auto it = state_->entries.find(e);
        const asset::AssetId base = mr.material.id();
        if (it == state_->entries.end() || it->second.base != base ||
            it->second.hash != override_hash(o, base)) {
            return true;
        }
    }
    return false;
}

std::expected<std::size_t, core::Error>
MaterialOverrideStore::resolve(entt::registry& r, rhi::GpuAllocator& allocator,
                               const rhi::TransferContext& transfer, std::uint64_t frame) {
    State& st = *state_;
    // Orphans: the entity lost its override, its mesh, or no longer exists
    // (destroyed, or a scene load replaced the registry).
    for (auto it = st.entries.begin(); it != st.entries.end();) {
        if (!has_active_override(r, it->first)) {
            st.retire(std::move(it->second.material), frame);
            it = st.entries.erase(it);
        } else {
            ++it;
        }
    }

    std::size_t rebuilt = 0;
    std::string failures;
    for (auto [e, mr, o] : r.view<const MeshRenderer, const MaterialOverride>().each()) {
        if (o.mask == 0 || !mr.material.valid()) continue;
        const asset::AssetId base_id = mr.material.id();
        const std::uint64_t hash = override_hash(o, base_id);
        Entry& entry = st.entries[e];
        if (entry.hash == hash && entry.base == base_id && entry.hash != 0) continue;

        const asset::MaterialAsset& base = *mr.material;
        auto derived = asset::derive_material(base, apply_material_override(base.params, o),
                                              allocator, transfer);
        st.retire(std::move(entry.material), frame);
        entry.material = derived ? std::make_shared<asset::MaterialAsset>(std::move(*derived))
                                 : nullptr;
        // Recorded even on failure, so a failing upload is not retried every
        // frame; the next edit retries it.
        entry.base = base_id;
        entry.hash = hash;
        ++rebuilt;
        if (!derived) {
            failures += (failures.empty() ? "" : "; ") + std::string("entity ") +
                        std::to_string(entt::to_integral(e)) + ": " + derived.error().message;
        }
    }
    if (!failures.empty()) {
        return std::unexpected(core::Error{"material override upload failed: " + failures});
    }
    return rebuilt;
}

void MaterialOverrideStore::collect(std::uint64_t frame) { (void)state_->retired.collect(frame); }

void MaterialOverrideStore::clear() {
    state_->entries.clear();
    state_->retired.flush();
}

std::size_t MaterialOverrideStore::live_count() const {
    std::size_t n = 0;
    for (const auto& [e, entry] : state_->entries) n += entry.material ? 1U : 0U;
    return n;
}

std::size_t MaterialOverrideStore::retired_count() const { return state_->retired.size(); }

} // namespace engine::scene
