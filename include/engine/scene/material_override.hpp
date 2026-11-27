#pragma once

// Material tuning (ROADMAP A8): the scalar PBR factors of the material an
// entity is drawn with, readable and writable per entity.
//
// A descriptor table (material_fields()) names every tunable factor of
// asset::PbrMaterialParams with its kind, range and a one-line hint; the codec
// (the `material` component of component_json, MaterialOverride), the remote
// material.get / material.set and the editor's Material inspector all go
// through it.
//
// Where an edit lands: a Primitive owns its colour, metallic, roughness and
// emission (they are its component fields), so set_entity_material_field()
// writes those to the Primitive; every other factor — and every factor of a
// glTF / code-created material — becomes a MaterialOverride on the entity.
// Overrides never modify the shared material asset: resolve_material_overrides()
// uploads a derived material (content-keyed in the AssetManager, like the
// primitives' materials) and the renderer draws it in place of the base.

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/core/error.hpp"

namespace engine::asset {
class AssetManager;
}
namespace engine::rhi {
class GpuAllocator;
struct TransferContext;
} // namespace engine::rhi

namespace engine::scene {

struct MaterialOverride;
struct MeshRenderer;

enum class MaterialFieldKind {
    color4, // linear RGBA in [0, 1]
    color3, // linear RGB in [min, max]
    scalar, // one float in [min, max]
};

struct MaterialFieldInfo {
    std::string_view  name;  // JSON key
    std::string_view  label; // editor widget label
    MaterialFieldKind kind;
    float             min = 0.0F;
    float             max = 1.0F;
    std::string_view  hint; // one line
    std::uint32_t     bit = 0; // MaterialOverride::mask bit
    // A Primitive's own component field (its edits go to the Primitive).
    bool primitive_owned = false;
};

// Every tunable factor, in display order.
[[nodiscard]] std::span<const MaterialFieldInfo> material_fields();
[[nodiscard]] const MaterialFieldInfo* find_material_field(std::string_view name);

// A field's value as a vec4 (colour3: w = 0, scalar: x).
[[nodiscard]] glm::vec4 material_field_value(const asset::PbrMaterialParams& params,
                                             const MaterialFieldInfo& field);
// Stores `value` (clamped into the field's range). Setting the transmission
// factor also sets / clears the material_transmission flag, which routes the
// material to the forward (glass) pass.
void set_material_field_value(asset::PbrMaterialParams& params, const MaterialFieldInfo& field,
                              const glm::vec4& value);

// `base` with the override's masked fields applied.
[[nodiscard]] asset::PbrMaterialParams apply_material_override(const asset::PbrMaterialParams& base,
                                                               const MaterialOverride& overrides);

// The parameters the entity's material is built from before its overrides:
// a Primitive's own material, else the MeshRenderer's material asset;
// nullopt when the entity has neither (or no material handle).
[[nodiscard]] std::optional<asset::PbrMaterialParams>
base_material_params(const entt::registry& registry, entt::entity entity);
// base_material_params() with the entity's MaterialOverride applied — what the
// entity renders with once the overrides are resolved.
[[nodiscard]] std::optional<asset::PbrMaterialParams>
effective_material_params(const entt::registry& registry, entt::entity entity);

// Edits one factor of the entity's material (see the header comment for
// where it lands). The entity must have a material (base_material_params).
void set_entity_material_field(entt::registry& registry, entt::entity entity,
                               const MaterialFieldInfo& field, const glm::vec4& value);
// Drops the override of one factor (the base value shows again); removes the
// MaterialOverride once it overrides nothing.
void clear_material_override_field(entt::registry& registry, entt::entity entity,
                                   const MaterialFieldInfo& field);

// ---- Codec (the `material` component) ----------------------------------------

// {"<field>": value, ...} for the overridden fields. Colours are linear arrays.
[[nodiscard]] nlohmann::json encode_material_override(const MaterialOverride& overrides);
// Every field of `params`, as encode_material_override() writes them.
[[nodiscard]] nlohmann::json encode_material_params(const asset::PbrMaterialParams& params);
// Reads one field's value: colours as a linear array or "#rrggbb" (sRGB),
// range-checked. The error message names what was expected.
[[nodiscard]] std::expected<glm::vec4, core::Error>
decode_material_field(const nlohmann::json& value, const MaterialFieldInfo& field);
// The descriptor table: [{name, label, kind: "color4"|"color3"|"scalar", min,
// max, hint, primitive_field}].
[[nodiscard]] nlohmann::json material_fields_schema();
// Merges `value` into `overrides`: a field with a value is overridden, a field
// set to null is no longer overridden, absent fields are kept. `strict`
// rejects unknown fields. Nothing changes on error.
[[nodiscard]] std::expected<void, core::Error>
decode_material_override(const nlohmann::json& value, MaterialOverride& overrides, bool strict);

// ---- Rendering -----------------------------------------------------------------

// The material asset to draw `entity` with: its resolved MaterialOverride's
// derived material when current, else the MeshRenderer's (null if unset).
[[nodiscard]] const asset::MaterialAsset* drawn_material(const entt::registry& registry,
                                                         entt::entity entity,
                                                         const MeshRenderer& renderer);

// True when some MaterialOverride's derived material is missing or stale.
// Cheap (no GPU access).
[[nodiscard]] bool material_overrides_need_resolve(const entt::registry& registry);

// (Re)builds the derived material of every stale MaterialOverride, under the
// AssetManager key "material_override/<base id>/<override hash>" (equal
// overrides of one base share a material). Returns the number rebuilt; an
// upload failure is reported after every entity was processed.
[[nodiscard]] std::expected<std::size_t, core::Error>
resolve_material_overrides(entt::registry& registry, rhi::GpuAllocator& allocator,
                           const rhi::TransferContext& transfer, asset::AssetManager& assets);

} // namespace engine::scene
