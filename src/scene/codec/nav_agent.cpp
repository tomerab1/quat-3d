// Codec component `nav_agent`: Navmesh path-following agent.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/navigation.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_nav_agent(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "nav_agent",
                         .summary = "Navmesh path-following agent",
                         .addable = true,
                         .writable = true,
                         .order = 1300};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* a = &r.get<NavAgent>(e);
        out["nav_agent"] = {{"target", vec3_json(a->target)},
                            {"speed", a->speed},
                            {"ground_offset", a->ground_offset},
                            {"active", a->active},
                            {"paused", a->paused}};
        return std::move(out["nav_agent"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "nav_agent";
        return merge<NavAgent>(r, e, v, name, strict, [](JsonFields& f, NavAgent& a) {
            const glm::vec3 old_target = a.target;
            f.vec3("target", a.target).number("speed", a.speed, 0.0F);
            f.number("ground_offset", a.ground_offset).boolean("active", a.active);
            f.boolean("paused", a.paused);
            if (a.target != old_target) { // a new walk: the system repaths
                a.commanded = true;          // ...and a route resumes after it
                a.arrived = false;
                a.failed = false;
                a.plan_failures = 0;
                a.retry_s = 0.0F;
                a.path.clear();
            }
        });
    };
    return registry.add(typed_codec<NavAgent>(std::move(codec)));
}

} // namespace engine::scene::codec
