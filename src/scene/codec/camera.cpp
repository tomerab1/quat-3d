// Codec component `camera`: Perspective camera.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/lens_json.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_camera(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "camera",
                         .summary = "Perspective camera",
                         .addable = true,
                         .writable = true,
                         .order = 800};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const Camera& c = r.get<Camera>(e);
        json out = {{"fov_deg", glm::degrees(c.fov_y)},
                    {"near", c.near_z},
                    {"far", c.far_z},
                    {"active", c.is_active}};
        write_lens_fields(out, c.lens);
        return out;
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "camera";
        return merge<Camera>(r, e, v, name, strict, [](JsonFields& f, Camera& c) {
            float fov_deg = glm::degrees(c.fov_y);
            f.number("fov_deg", fov_deg, 1.0F, 179.0F);
            if (f.has("fov_y")) { // legacy scene files: radians
                float fov_rad = c.fov_y;
                f.number("fov_y", fov_rad, glm::radians(1.0F), glm::radians(179.0F));
                fov_deg = glm::degrees(fov_rad);
            }
            c.fov_y = glm::radians(fov_deg);
            f.number("near", c.near_z, 1e-4F).number("far", c.far_z, 1e-3F).boolean("active", c.is_active);
            if (c.far_z <= c.near_z) f.fail("far", "must be greater than near");
            read_lens_fields(f, c.lens); // R20 physical lens
        });
    };
    return registry.add(typed_codec<Camera>(std::move(codec)));
}

} // namespace engine::scene::codec
