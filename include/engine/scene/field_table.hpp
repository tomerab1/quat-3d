#pragma once

// Field tables (ROADMAP G17.1): a component described field by field, from
// which the engine derives everything it needs without per-component code:
// the JSON codec (component_registry.hpp), the remote JSON Schema
// (scene.components), Lua access (through the codec) and the editor's
// inspector rows. Game modules describe their components this way
// (engine::game::ComponentFields<T>, engine/game/fields.hpp), since games
// never include ImGui.
//
// FieldTable is the type-erased form the registry and the editor read.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <nlohmann/json_fwd.hpp>

namespace engine::core {
class JsonFields;
}

namespace engine::scene {

enum class FieldKind : std::uint8_t {
    boolean, // bool
    integer, // std::int32_t
    number,  // float
    vec2,    // glm::vec2
    vec3,    // glm::vec3
    color3,  // glm::vec3, linear RGB (JSON also accepts "#rrggbb")
    color4,  // glm::vec4, linear RGBA (JSON also accepts "#rrggbbaa")
    string,  // std::string
    choice,  // std::int32_t index into `choices`, written as the choice's name
};

struct FieldDesc {
    std::string name{};        // JSON key: lowercase letters, digits, '_'
    FieldKind   kind = FieldKind::number;
    std::string description{}; // one line (schema description, inspector tooltip)
    // Inclusive range for integer / number / vec components (clamped in the
    // inspector, an error in JSON).
    double min = -1.0e30;
    double max = 1.0e30;
    std::size_t              max_length = 1024; // string
    std::vector<std::string> choices{};         // choice
    bool                     read_only = false;  // encoded, never decoded or edited
};

struct FieldTable {
    std::vector<FieldDesc> fields{};
    // The component of `entity`, or null. One accessor per field gives the
    // address of that field inside it, typed by the field's kind.
    std::function<void*(entt::registry&, entt::entity)> component{};
    std::vector<std::function<void*(void* component)>>  address{};
};

// The fields of `component` (the table's component) as a JSON object.
[[nodiscard]] nlohmann::json encode_field_table(const FieldTable& table, const void* component);
// Reads the fields present in `fields` into `component` with the table's
// type and range checks; errors go to `fields` (field_name: message).
void decode_field_table(core::JsonFields& fields, const FieldTable& table, void* component);

// The JSON Schema (draft 2020-12 subset) of a field table: an object with one
// property per field, additionalProperties false.
[[nodiscard]] nlohmann::json field_table_schema(const FieldTable& table, std::string_view title);

} // namespace engine::scene
