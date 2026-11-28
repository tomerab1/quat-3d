// The sdk-consumer game's native module (ROADMAP G17.1): its Spinner as a
// field-table component, spinner_system in the pre_physics phase and one
// remote method with a JSON Schema, through engine::game::Registrar — built
// against the installed SDK only, so a break in the module API fails here.

#include <nlohmann/json.hpp>

#include "engine/game/module.hpp"
#include "spinner.hpp"

namespace consumer {

namespace {

void spin(engine::game::SystemContext& context, float dt) { spinner_system(context.registry(), dt); }

} // namespace

std::expected<void, engine::core::Error> register_module(engine::game::Registrar& registrar) {
    registrar.component<Spinner>(
        "consumer_spinner", "Spins its entity about +Y (sdk-consumer)",
        engine::game::ComponentFields<Spinner>()
            .field("degrees_per_second", &Spinner::degrees_per_second, {.min = -3600.0, .max = 3600.0})
            .field("angle_degrees", &Spinner::angle_degrees, {.read_only = true}));
    registrar.system({.name = "consumer.spin", .phase = engine::game::Phase::pre_physics, .run = &spin});
    registrar.method({.name = "consumer.count",
                      .summary = "How many spinners the scene has: {} -> {count}.",
                      .params_schema = {{"type", "object"}, {"additionalProperties", false}},
                      .handler = [](engine::game::SystemContext& context, const nlohmann::json&)
                          -> std::expected<nlohmann::json, engine::core::Error> {
                          return nlohmann::json{{"count", context.registry().view<Spinner>().size()}};
                      }});
    return {};
}

} // namespace consumer
