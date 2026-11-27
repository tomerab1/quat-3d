// render.settings.* (the renderer's tunable parameters) and material.* (the
// PBR factors an entity is drawn with) — ROADMAP A8.

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/render_settings_json.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

// ---- render.settings.* -----------------------------------------------------------

[[nodiscard]] std::expected<renderer::RenderSettings*, RpcError> settings_of(Context& ctx) {
    if (ctx.services.render_settings == nullptr) return unavailable("the renderer's settings");
    return ctx.services.render_settings;
}

// Optional `group` parameter: empty = every group.
[[nodiscard]] std::expected<std::string, RpcError> group_param(const Json& params) {
    core::JsonFields f(params, "");
    std::string group;
    f.string("group", group, 64);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!group.empty() && renderer::find_render_setting_group(group) == nullptr) {
        std::string known;
        for (const renderer::RenderSettingGroup& g : renderer::render_setting_groups()) {
            known += (known.empty() ? "" : ", ") + std::string(g.name);
        }
        return bad_params("group: unknown group '" + group + "' (known: " + known + ")");
    }
    return group;
}

RpcResult settings_get(Context& ctx, const Json& params) {
    auto group = group_param(params);
    if (!group) return std::unexpected(group.error());
    auto settings = settings_of(ctx);
    if (!settings) return std::unexpected(settings.error());
    return scene::encode_render_settings(**settings, *group);
}

RpcResult settings_set(Context& ctx, const Json& params) {
    if (!params.is_object()) return bad_params("expected {<group>: {<field>: value}}");
    auto settings = settings_of(ctx);
    if (!settings) return std::unexpected(settings.error());
    if (auto ok = scene::apply_render_settings_json(**settings, params, /*strict=*/true); !ok) {
        return bad_params(ok.error().message);
    }
    return scene::encode_render_settings(**settings);
}

RpcResult settings_reset(Context& ctx, const Json& params) {
    auto group = group_param(params);
    if (!group) return std::unexpected(group.error());
    auto settings = settings_of(ctx);
    if (!settings) return std::unexpected(settings.error());
    (void)renderer::reset_render_settings(**settings, *group);
    return scene::encode_render_settings(**settings);
}

RpcResult settings_schema(Context& /*ctx*/, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    Json out = scene::render_settings_schema();
    out["material"] = scene::material_fields_schema();
    return out;
}

// ---- material.* --------------------------------------------------------------------

// The entity named by the required `entity` parameter, checked to have a
// material that material.* can tune.
[[nodiscard]] std::expected<entt::entity, RpcError> material_entity(Context& ctx,
                                                                    core::JsonFields& f) {
    const entt::registry& r = ctx.scene->registry();
    auto e = required_entity(f, r, "entity");
    if (!e) return e;
    if (r.all_of<scene::InstancedMesh>(*e)) {
        return bad_params("entity: an instanced mesh keeps its material in its instanced_mesh "
                          "component (edit it with scene.update)");
    }
    if (!scene::base_material_params(r, *e)) {
        return bad_params("entity: it has no material (needs a primitive or a mesh)");
    }
    return e;
}

[[nodiscard]] Json texture_slots(std::uint32_t flags) {
    struct Slot {
        std::uint32_t bit;
        const char*   name;
    };
    constexpr Slot slots[] = {{asset::material_has_base_color, "base_color"},
                              {asset::material_has_normal, "normal"},
                              {asset::material_has_metallic_roughness, "metallic_roughness"},
                              {asset::material_has_emissive, "emissive"},
                              {asset::material_has_occlusion, "occlusion"},
                              {asset::material_has_thickness, "thickness"}};
    Json out = Json::array();
    for (const Slot& s : slots) {
        if ((flags & s.bit) != 0) out.push_back(s.name);
    }
    return out;
}

[[nodiscard]] Json material_json(const entt::registry& r, entt::entity e) {
    const asset::PbrMaterialParams base = *scene::base_material_params(r, e);
    const asset::PbrMaterialParams params = *scene::effective_material_params(r, e);
    const bool primitive = r.all_of<scene::Primitive>(e);
    Json overridden = Json::array();
    if (const auto* o = r.try_get<scene::MaterialOverride>(e)) {
        for (const scene::MaterialFieldInfo& f : scene::material_fields()) {
            if ((o->mask & f.bit) != 0) overridden.push_back(f.name);
        }
    }
    const auto* name = r.try_get<scene::Name>(e);
    Json out = {{"id", entity_json(e)},
                {"name", name != nullptr ? name->value : std::string()},
                {"source", primitive ? "primitive" : "mesh"},
                {"material", scene::encode_material_params(params)},
                {"overridden", std::move(overridden)},
                {"textures", texture_slots(base.flags)},
                {"transparent", (params.flags & (asset::material_blend |
                                                 asset::material_transmission)) != 0}};
    if (!primitive) {
        // Entities drawing the same material asset (a set on this one does not
        // change them).
        std::size_t shared = 0;
        const asset::AssetId id = r.get<scene::MeshRenderer>(e).material.id();
        for (auto [other, mr] : r.view<const scene::MeshRenderer>().each()) {
            if (mr.material.valid() && mr.material.id() == id) ++shared;
        }
        out["shared_by"] = shared;
    }
    return out;
}

RpcResult material_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = material_entity(ctx, f);
    if (!e) return std::unexpected(e.error());
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    return material_json(ctx.scene->registry(), *e);
}

RpcResult material_set(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = material_entity(ctx, f);
    if (!e) return std::unexpected(e.error());
    const Json* changes = f.raw("material");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (changes == nullptr || !changes->is_object()) {
        return bad_params("material: expected an object of {<field>: value | null}");
    }

    // Decode every field before changing anything.
    struct Change {
        const scene::MaterialFieldInfo* field;
        std::optional<glm::vec4>        value; // nullopt: drop the override
    };
    std::vector<Change> plan;
    for (const auto& [key, value] : changes->items()) {
        const scene::MaterialFieldInfo* field = scene::find_material_field(key);
        if (field == nullptr) {
            std::string known;
            for (const scene::MaterialFieldInfo& m : scene::material_fields()) {
                known += (known.empty() ? "" : ", ") + std::string(m.name);
            }
            return bad_params("material." + key + ": unknown field (known: " + known + ")");
        }
        if (value.is_null()) {
            plan.push_back({field, std::nullopt});
            continue;
        }
        auto decoded = scene::decode_material_field(value, *field, /*strict=*/true);
        if (!decoded) return bad_params("material." + key + ": " + decoded.error().message);
        plan.push_back({field, *decoded});
    }

    entt::registry& r = ctx.scene->registry();
    for (const Change& c : plan) {
        if (c.value) {
            scene::set_entity_material_field(r, *e, *c.field, *c.value);
        } else {
            scene::clear_material_override_field(r, *e, *c.field);
        }
    }
    return material_json(r, *e);
}

} // namespace

void register_tuning_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("render.settings.get",
                      "Renderer settings {<group>: {<field>: value}}: every group, or {group} only.",
                      [ctx](const Json& p) { return settings_get(*ctx, p); });
    server.add_method("render.settings.set",
                      "Merge {<group>: {<field>: value}} into the renderer settings (range-checked, "
                      "all or nothing); returns every group.",
                      [ctx](const Json& p) { return settings_set(*ctx, p); });
    server.add_method("render.settings.reset",
                      "Restore the defaults of {group} (every group when omitted); returns every "
                      "group.",
                      [ctx](const Json& p) { return settings_reset(*ctx, p); });
    server.add_method("render.settings.schema",
                      "Descriptor table: {groups:[{name, label, hint, fields:[{name, type, min, "
                      "max, default, unit, hint}]}], material:[{name, kind, min, max, hint}]}.",
                      [ctx](const Json& p) { return settings_schema(*ctx, p); });
    server.add_method("material.get",
                      "Material factors an entity is drawn with: {entity} -> {id, name, source, "
                      "material:{...}, overridden, textures, transparent, shared_by?}.",
                      [ctx](const Json& p) { return material_get(*ctx, p); });
    server.add_method("material.set",
                      "Tune {entity, material:{<field>: value | null}} (null reverts an override); "
                      "per entity, returns material.get.",
                      [ctx](const Json& p) { return material_set(*ctx, p); });
}

} // namespace engine::remote::world
