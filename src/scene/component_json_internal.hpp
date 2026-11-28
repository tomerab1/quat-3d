#pragma once

// Private to the component codec: the helpers the per-component codec files
// (src/scene/codec/*.cpp, ROADMAP S9) share, and the codecs that live in
// their own files. component_json_common.cpp defines the non-template ones.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"

namespace engine::renderer {
struct ParticleEmitterSettings;
}

namespace engine::scene {
struct Primitive;
}

namespace engine::scene::detail {

using json = nlohmann::json;

// Asset paths in components (glTF files, clips) are at most this long.
inline constexpr std::size_t k_max_asset_path = 4096;
// Placements one instanced mesh may hold (hand-placed or scattered).
inline constexpr std::size_t k_max_instances = 100'000;

template <typename E, std::size_t N>
[[nodiscard]] std::string enum_name(const core::EnumName<E> (&names)[N], E value) {
    for (const auto& n : names) {
        if (n.value == value) return std::string(n.name);
    }
    return "unknown";
}

[[nodiscard]] inline json vec3_json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
[[nodiscard]] inline json vec4_json(const glm::vec4& v) {
    return json::array({v.x, v.y, v.z, v.w});
}
[[nodiscard]] inline json range_json(const glm::vec2& r) { return json::array({r.x, r.y}); }

// Decodes `v` into a copy of the entity's T (or a default T), and stores it
// only when every field was valid.
template <typename T, typename Decode>
[[nodiscard]] std::expected<void, core::Error> merge(entt::registry& r, entt::entity e,
                                                     const json& v, std::string_view name,
                                                     bool strict, Decode&& decode) {
    T component = r.all_of<T>(e) ? r.get<T>(e) : T{};
    core::JsonFields f(v, std::string(name), strict);
    decode(f, component);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    r.emplace_or_replace<T>(e, std::move(component));
    return {};
}

// The common codec shape: T present = has it; encode(const T&) -> json;
// apply = merge with decode(JsonFields&, T&); remove = remove<T>.
template <typename T, typename Encode, typename Decode>
[[nodiscard]] ComponentCodec field_codec(ComponentCodec base, Encode encode, Decode decode) {
    base.encode = [encode](const entt::registry& r, entt::entity e, const EncodeOptions&) {
        return json(encode(r.get<T>(e)));
    };
    base.apply = [decode, name = base.name](entt::registry& r, entt::entity e, const json& v,
                                            bool strict) {
        return merge<T>(r, e, v, name, strict, decode);
    };
    return typed_codec<T>(std::move(base));
}

// Fails `key` unless `path` is empty or relative to the project root without
// climbing out of it (remote clients write these paths).
void check_project_path(core::JsonFields& f, std::string_view key, const std::string& path);

// Reads a [min, max] pair within [lo, hi] (min <= max).
void read_range(core::JsonFields& f, std::string_view key, glm::vec2& out, float lo, float hi);

// A colour field as decode_color accepts it; `out` keeps its value on error.
void read_color(core::JsonFields& f, std::string_view key, glm::vec4& out);
void read_color3(core::JsonFields& f, std::string_view key, glm::vec3& out);

// The shape + flat material fields shared by `primitive` and `instanced_mesh`.
void read_primitive_fields(core::JsonFields& f, Primitive& p);
[[nodiscard]] json primitive_fields_json(const Primitive& p);

// Editing a physics component invalidates its body / character (or, for a
// joint, only its constraint) so the physics systems rebuild it.
void invalidate_physics(entt::registry& r, entt::entity e, std::string_view edited);

// particle_emitter (particle_json.cpp).
[[nodiscard]] json encode_particle_emitter(const renderer::ParticleEmitterSettings& s);
void decode_particle_emitter(core::JsonFields& f, renderer::ParticleEmitterSettings& s);

} // namespace engine::scene::detail
