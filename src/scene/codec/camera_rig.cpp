// Codec component `camera_rig`: Orbit / strategy camera rig driving the entity: focus, yaw, pitch eased with zoom, distance, springs, ground following, bounds.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_camera_rig.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;

} // namespace

std::expected<void, core::Error> register_camera_rig(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "camera_rig",
                         .summary = "Orbit / strategy camera rig driving the entity: focus, yaw, pitch eased with zoom, distance, springs, ground following, bounds",
                         .addable = true,
                         .writable = true,
                         .order = 2700};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const CameraRig& c = r.get<CameraRig>(e);
        return json(detail::encode_camera_rig(c));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "camera_rig";
        return merge<CameraRig>(r, e, v, name, strict, detail::decode_camera_rig);
    };
    return registry.add(typed_codec<CameraRig>(std::move(codec)));
}

} // namespace engine::scene::codec
