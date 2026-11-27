#pragma once

// Component JSON codec (ROADMAP S2 / A2).
//
// One encoder/decoder per serializable component, shared by scene files
// (scene_io) and the remote-control API, so both speak the same vocabulary
// and every read is checked: the engine builds without exceptions, and scene
// files and remote requests are untrusted, so decoding goes through
// core::JsonFields (type + range checks, precise error paths) — a bad value
// is an error, never an abort.
//
// Component names (the JSON keys):
//   transform        position / rotation (Euler degrees, YXZ) / scale, or a 16-float matrix
//   primitive        procedural shape + flat PBR material
//   material         per-entity overrides of the drawn material's PBR factors
//                    (material_override.hpp); null clears a field
//   instanced_mesh   glTF (or primitive) model + placements (omitted when a
//                    scatter generates them; instance_count is read-only)
//   scatter          seeded procedural placement of the instanced mesh
//   point_light, dir_light, camera
//   collider, rigidbody, character
//   nav_agent, behavior, patrol
//   script           Lua gameplay script path + property overrides (G7)
//   terrain
//   grass            GPU grass around the camera (density, blades, colours, wind, limits, LOD)
//   net_replicated
//   audio_source     clip / bus / volume / pitch / loop / 3D falloff (+ write-only command)
//   audio_listener
//   animator         edit-only (needs a clip from a glTF; cannot be added):
//                    clip (by name, switches with blend / blend_time), time, speed, looping
//   foot_ik          foot placement (joint names, probe lengths, limits, smoothing)
//   mesh             read-only (glTF provenance; scene_io resolves it)
//
// Decoding merges: fields present in the JSON overwrite the component's
// current values (or the defaults when the entity lacks it), absent fields
// are kept. Enums are written as strings and also read from the legacy
// integer encoding of scene-file version 1.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {

struct ComponentInfo {
    std::string_view name;
    std::string_view summary;  // one line, for tool/API listings
    bool             addable;  // false: edit-only or read-only
    bool             writable; // false: read-only (encode only)
};

// Every codec component, in canonical order.
[[nodiscard]] std::span<const ComponentInfo> component_infos();
[[nodiscard]] const ComponentInfo* find_component_info(std::string_view name);

struct EncodeOptions {
    // Scene files keep the exact local matrix; the remote API prefers
    // human-readable position / rotation / scale.
    bool transform_as_matrix = false;
    // Include read-only components (mesh provenance).
    bool include_read_only = true;
    // Non-empty: encode only the components named here (unknown names are
    // ignored), skipping the cost of the rest (instance lists can be long).
    std::span<const std::string> only{};
};

// Every codec component the entity carries (or those of `options.only`), as
// {"<name>": {...}, ...}.
[[nodiscard]] nlohmann::json encode_components(const entt::registry& registry,
                                               entt::entity entity,
                                               const EncodeOptions& options = {});

// Names of the codec components the entity carries, in canonical order —
// the cheap form of encode_components(...).items() for listings and filters.
[[nodiscard]] std::vector<std::string_view> component_names(const entt::registry& registry,
                                                            entt::entity entity,
                                                            bool include_read_only = true);

// Add or merge-update component `name` on `entity` from `value`. `strict`
// rejects unknown fields (remote requests); scene files decode leniently so
// newer files still load in older builds. Read-only components are refused.
[[nodiscard]] std::expected<void, core::Error>
apply_component(entt::registry& registry, entt::entity entity, std::string_view name,
                const nlohmann::json& value, bool strict);

// Decodes a colour as the codec accepts it everywhere: "#rrggbb" /
// "#rrggbbaa" (sRGB, converted to linear) or a linear [r, g, b] / [r, g, b, a]
// array in 0..1 (a 3-element array keeps `out`'s alpha). `out` is untouched
// on error.
[[nodiscard]] std::expected<void, core::Error> decode_color(const nlohmann::json& value,
                                                            glm::vec4& out);

// Remove component `name` (and what it implies: removing `primitive` also
// removes the MeshRenderer it produced). Returns false for an unknown name or
// a component that cannot be removed (transform).
bool remove_component(entt::registry& registry, entt::entity entity, std::string_view name);

} // namespace engine::scene
