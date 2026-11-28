// Codec component `patrol`: Patrol route the nav agent walks.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_patrol(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "patrol",
                         .summary = "Patrol route the nav agent walks",
                         .addable = true,
                         .writable = true,
                         .order = 1500};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const PatrolRoute& p = r.get<PatrolRoute>(e);
        json pts = json::array();
        for (const glm::vec3& pt : p.points) pts.push_back(vec3_json(pt));
        return json({{"points", std::move(pts)}, {"loop", p.loop}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "patrol";
        return merge<PatrolRoute>(r, e, v, name, strict, [](JsonFields& f, PatrolRoute& p) {
            if (f.has("points")) {
                p.next = 0;
                p.finished = false;
            }
            f.vec3_list("points", p.points).boolean("loop", p.loop);
        });
    };
    return registry.add(typed_codec<PatrolRoute>(std::move(codec)));
}

} // namespace engine::scene::codec
