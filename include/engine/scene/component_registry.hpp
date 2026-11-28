#pragma once

// The component registry (ROADMAP S9): the table behind the component JSON
// codec (component_json.hpp). Each codec component registers one
// ComponentCodec; encode_components / component_names / apply_component /
// remove_component and component_infos() dispatch through the registry, so
// a new component is one new file that registers itself, never an edit to a
// central list.
//
// Engine components live one per file in src/scene/codec/<name>.cpp, each
// defining engine::scene::codec::register_<name>(ComponentRegistry&); the
// build generates the call list from that directory
// (cmake/QuatRegistration.cmake). Game modules (G17.1) add theirs at boot.
//
// The registry is filled at startup on the main thread, before scenes are
// loaded; spans and pointers it returns stay valid until the next add() or
// remove().

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/registry.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/field_table.hpp"

namespace engine::scene {

struct ComponentCodec {
    // The JSON key: lowercase letters, digits and '_', starting with a letter.
    std::string name{};
    std::string summary{}; // one line, for tool / API listings
    bool        addable = true;  // false: edit-only or read-only
    bool        writable = true; // false: read-only (encode only; needs no apply)
    // Canonical position (ascending; ties keep registration order). Scene
    // files, scripts and listings visit components in this order, so a
    // component that refers to another one sorts after it. Engine components
    // use 100..9999 (src/scene/codec/README.md); game components default
    // after every engine one.
    std::int32_t order = k_default_order;

    // Whether the entity carries the component (as the codec sees it: an
    // empty material override counts as absent). Required.
    std::function<bool(const entt::registry&, entt::entity)> has{};
    // Its JSON. Called only when has() is true. Required.
    std::function<nlohmann::json(const entt::registry&, entt::entity, const EncodeOptions&)> encode{};
    // Add or merge-update from JSON (see apply_component). Required when writable.
    std::function<std::expected<void, core::Error>(entt::registry&, entt::entity,
                                                   const nlohmann::json&, bool strict)>
        apply{};
    // Remove it and what it implies; returns whether it was there. Null: the
    // component cannot be removed (transform).
    std::function<bool(entt::registry&, entt::entity)> remove{};

    // Optional: the component's JSON Schema (scene.components reports it) and
    // its field table (the editor draws inspector rows from it). Components
    // described by a field table (engine::game::ComponentFields) have both.
    std::shared_ptr<const nlohmann::json> schema{};
    std::shared_ptr<const FieldTable>     fields{};

    static constexpr std::int32_t k_default_order = 100'000;
};

class ComponentRegistry {
public:
    ComponentRegistry();
    ~ComponentRegistry();
    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;

    // Registers `codec`. Refuses an invalid or duplicate name and a codec
    // missing a required function, with a message naming the problem.
    [[nodiscard]] std::expected<void, core::Error> add(ComponentCodec codec);
    // Unregisters `name` (tests, and modules that registered it); false when unknown.
    bool remove(std::string_view name);

    // Every component, in canonical order.
    [[nodiscard]] std::span<const ComponentInfo> infos() const { return infos_; }
    [[nodiscard]] std::size_t size() const { return infos_.size(); }
    [[nodiscard]] const ComponentCodec* find(std::string_view name) const;
    // The codec at canonical position `index` (< size()).
    [[nodiscard]] const ComponentCodec& at(std::size_t index) const;

private:
    void rebuild_infos();

    std::vector<std::unique_ptr<ComponentCodec>> codecs_; // canonical order
    std::vector<ComponentInfo>                   infos_;  // views into codecs_
};

// The process-wide registry the codec functions use, holding the engine's
// components from the first call on.
[[nodiscard]] ComponentRegistry& component_registry();

// Registers the engine's own components (src/scene/codec/*.cpp) into
// `registry`: what component_registry() starts with.
[[nodiscard]] std::expected<void, core::Error> register_engine_components(ComponentRegistry& registry);

// Names of the engine component files the build generated registration for
// (the stems of src/scene/codec/*.cpp), sorted: the registration's own record
// of what it found, for tests.
[[nodiscard]] std::span<const std::string_view> engine_component_sources();

// A codec for component type T whose presence is `all_of<T>` and whose
// removal is `remove<T>`: fills `has` and `remove` when unset.
template <typename T>
[[nodiscard]] ComponentCodec typed_codec(ComponentCodec codec);

template <typename T>
ComponentCodec typed_codec(ComponentCodec codec) {
    if (!codec.has) {
        codec.has = [](const entt::registry& r, entt::entity e) { return r.all_of<T>(e); };
    }
    if (!codec.remove) {
        codec.remove = [](entt::registry& r, entt::entity e) { return r.remove<T>(e) > 0; };
    }
    return codec;
}

} // namespace engine::scene
