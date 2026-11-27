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
// Overrides never modify the shared material asset: a MaterialOverrideStore
// (owned by the renderer) builds one derived material per entity — a copy of
// the base with the overridden parameters — and the renderer draws it in place
// of the base.

#include <cstdint>
#include <expected>
#include <memory>
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
// Reads one field's value: colours as a linear array or "#rrggbb" (sRGB).
// `strict` (remote requests) rejects values outside the field's range;
// lenient (scene files) clamps them. Wrong types are always errors; the
// message names what was expected.
[[nodiscard]] std::expected<glm::vec4, core::Error>
decode_material_field(const nlohmann::json& value, const MaterialFieldInfo& field, bool strict);
// The descriptor table: [{name, label, kind: "color4"|"color3"|"scalar", min,
// max, hint, primitive_field}].
[[nodiscard]] nlohmann::json material_fields_schema();
// Merges `value` into `overrides`: a field with a value is overridden, a field
// set to null is no longer overridden, absent fields are kept. `strict`
// rejects unknown fields and out-of-range values (lenient: ignored / clamped,
// as for scene files). Nothing changes on error.
[[nodiscard]] std::expected<void, core::Error>
decode_material_override(const nlohmann::json& value, MaterialOverride& overrides, bool strict);

// ---- Rendering -----------------------------------------------------------------

// Owns the derived material of every entity with a MaterialOverride: one per
// entity, rebuilt when the overrides or the base material change. A replaced
// material — and the material of an entity that lost its override, was
// destroyed or belongs to a replaced scene — is retired and destroyed only
// once no frame in flight can still read it, so the live derived materials
// never exceed one per overridden entity plus those retired within the last
// `frames_in_flight` frames. Main thread only; destroy (or clear()) it only
// with the device idle.
class MaterialOverrideStore {
public:
    explicit MaterialOverrideStore(std::uint32_t frames_in_flight = 2);
    ~MaterialOverrideStore();
    MaterialOverrideStore(MaterialOverrideStore&&) noexcept;
    MaterialOverrideStore& operator=(MaterialOverrideStore&&) noexcept;
    MaterialOverrideStore(const MaterialOverrideStore&) = delete;
    MaterialOverrideStore& operator=(const MaterialOverrideStore&) = delete;

    // The material to draw `entity` with: its derived material while it is
    // current for the entity's overrides and base material, else the
    // MeshRenderer's (null if unset).
    [[nodiscard]] const asset::MaterialAsset* drawn(const entt::registry& registry,
                                                   entt::entity entity,
                                                   const MeshRenderer& renderer) const;

    // True when resolve() has work: a derived material is missing or stale,
    // or one belongs to an entity that no longer has an override. Cheap (no
    // GPU access).
    [[nodiscard]] bool needs_resolve(const entt::registry& registry) const;

    // Rebuilds stale derived materials and retires the replaced and orphaned
    // ones. `frame` numbers the frame about to be recorded (monotonic; the
    // frame loop's presented-frame count). Returns the number rebuilt; upload
    // failures are reported after every entity was processed (that entity
    // draws its base material until its overrides change).
    [[nodiscard]] std::expected<std::size_t, core::Error>
    resolve(entt::registry& registry, rhi::GpuAllocator& allocator,
            const rhi::TransferContext& transfer, std::uint64_t frame);

    // Destroys retired materials no frame in flight can read any more: call
    // once per frame with the number of the frame about to be recorded,
    // after the previous frame's slot fence was waited.
    void collect(std::uint64_t frame);

    // Destroys everything now (the device must be idle).
    void clear();

    [[nodiscard]] std::size_t live_count() const;    // current derived materials
    [[nodiscard]] std::size_t retired_count() const; // awaiting release

private:
    struct Entry;
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace engine::scene
