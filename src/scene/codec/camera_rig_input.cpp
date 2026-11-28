// Codec component `camera_rig_input`: Input actions that drive the entity's camera_rig (pan, orbit, zoom to cursor, grab-the-world drag).
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

std::expected<void, core::Error> register_camera_rig_input(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "camera_rig_input",
                         .summary = "Input actions that drive the entity's camera_rig (pan, orbit, zoom to cursor, grab-the-world drag)",
                         .addable = true,
                         .writable = true,
                         .order = 2800};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const CameraRigInput& c = r.get<CameraRigInput>(e);
        return json(detail::encode_camera_rig_input(c));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "camera_rig_input";
        return merge<CameraRigInput>(r, e, v, name, strict, detail::decode_camera_rig_input);
    };
    return registry.add(typed_codec<CameraRigInput>(std::move(codec)));
}

} // namespace engine::scene::codec
