#pragma once

// ComponentFields<T> (ROADMAP G17.1): a game component described field by
// field. Registrar::component turns it into the component's JSON codec,
// remote JSON Schema, Lua access (through the codec) and editor inspector
// rows, so a game writes no codec or UI code (and never includes ImGui).
//
//   struct Lantern { float range = 6.0F; glm::vec3 tint{1.0F}; bool lit = true; };
//   registrar.component<Lantern>("cozy_lantern", "A lantern on a post",
//       engine::game::ComponentFields<Lantern>()
//           .field("range", &Lantern::range, {.min = 0.5, .max = 50.0, .description = "Light radius, m"})
//           .color("tint", &Lantern::tint)
//           .field("lit", &Lantern::lit));
//
// Field types: bool, std::int32_t, float, glm::vec2, glm::vec3, glm::vec4
// (colour), std::string, and std::int32_t choices (.choice()).

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/scene/field_table.hpp"

namespace engine::game {

struct FieldOptions {
    double      min = -1.0e30;
    double      max = 1.0e30;
    std::string description{};
    std::size_t max_length = 1024; // strings
    bool        read_only = false;
};

template <typename T>
class ComponentFields {
public:
    // A plain field; its kind follows the member's type.
    template <typename M>
    ComponentFields& field(std::string_view name, M T::*member, FieldOptions options = {}) {
        if constexpr (std::same_as<M, bool>) {
            return add(name, member, scene::FieldKind::boolean, std::move(options));
        } else if constexpr (std::same_as<M, std::int32_t>) {
            return add(name, member, scene::FieldKind::integer, std::move(options));
        } else if constexpr (std::same_as<M, float>) {
            return add(name, member, scene::FieldKind::number, std::move(options));
        } else if constexpr (std::same_as<M, glm::vec2>) {
            return add(name, member, scene::FieldKind::vec2, std::move(options));
        } else if constexpr (std::same_as<M, glm::vec3>) {
            return add(name, member, scene::FieldKind::vec3, std::move(options));
        } else if constexpr (std::same_as<M, glm::vec4>) {
            return add(name, member, scene::FieldKind::color4, std::move(options));
        } else if constexpr (std::same_as<M, std::string>) {
            return add(name, member, scene::FieldKind::string, std::move(options));
        } else {
            static_assert(sizeof(M) == 0, "field types: bool, std::int32_t, float, glm::vec2, "
                                          "glm::vec3, glm::vec4, std::string");
        }
    }
    // A linear RGB colour (glm::vec3); JSON also takes "#rrggbb".
    ComponentFields& color(std::string_view name, glm::vec3 T::*member, FieldOptions options = {}) {
        return add(name, member, scene::FieldKind::color3, std::move(options));
    }
    // An index into `choices`, written and read as the choice's name.
    ComponentFields& choice(std::string_view name, std::int32_t T::*member,
                            std::vector<std::string> choices, FieldOptions options = {}) {
        add(name, member, scene::FieldKind::choice, std::move(options));
        table_->fields.back().choices = std::move(choices);
        return *this;
    }

    // The type-erased table (Registrar::component uses it).
    [[nodiscard]] std::shared_ptr<scene::FieldTable> table() const {
        auto table = std::make_shared<scene::FieldTable>(*table_);
        table->component = [](entt::registry& r, entt::entity e) -> void* { return r.try_get<T>(e); };
        return table;
    }

private:
    template <typename M>
    ComponentFields& add(std::string_view name, M T::*member, scene::FieldKind kind,
                         FieldOptions options) {
        table_->fields.push_back({.name = std::string(name),
                                  .kind = kind,
                                  .description = std::move(options.description),
                                  .min = options.min,
                                  .max = options.max,
                                  .max_length = options.max_length,
                                  .choices = {},
                                  .read_only = options.read_only});
        table_->address.emplace_back(
            [member](void* component) -> void* { return &(static_cast<T*>(component)->*member); });
        return *this;
    }

    std::shared_ptr<scene::FieldTable> table_ = std::make_shared<scene::FieldTable>();
};

} // namespace engine::game
