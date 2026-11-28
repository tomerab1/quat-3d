// Coin Collector's native module — see coin_module.hpp.

#define GLM_ENABLE_EXPERIMENTAL
#include "coin_module.hpp"

#include <cmath>
#include <string_view>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/components.hpp"

namespace coin_collector {

namespace {

using json = nlohmann::json;
using engine::core::JsonFields;

// A fixed phase per coin (the Lua version drew a random one): the golden
// ratio spreads consecutive entity ids around the circle.
[[nodiscard]] float initial_phase(entt::entity e) {
    const auto id = static_cast<float>(entt::to_entity(e));
    const float turns = id * 0.6180339887F;
    return (turns - std::floor(turns)) * glm::two_pi<float>();
}

} // namespace

void spin_coins(engine::game::SystemContext& context, float dt) {
    entt::registry& registry = context.registry();
    for (auto [e, transform, coin] : registry.view<engine::scene::Transform, CoinSpin>().each()) {
        if (!coin.anchored) {
            coin.base = glm::vec3(transform.local[3]);
            coin.scale = {glm::length(glm::vec3(transform.local[0])),
                          glm::length(glm::vec3(transform.local[1])),
                          glm::length(glm::vec3(transform.local[2]))};
            coin.phase = initial_phase(e);
            coin.anchored = true;
        }
        coin.phase = std::fmod(coin.phase + dt * coin.bob_rate, glm::two_pi<float>());
        coin.angle = std::fmod(coin.angle + coin.spin_speed * dt, 360.0F);
        const glm::vec3 position = coin.base + glm::vec3(0.0F, std::sin(coin.phase) * coin.bob_height, 0.0F);
        // Stand the coin up (pitch 90) and spin it about the vertical axis
        // (the engine's Euler order, YXZ).
        transform.local = glm::translate(glm::mat4(1.0F), position) *
                          glm::eulerAngleYXZ(glm::radians(coin.angle), glm::radians(90.0F), 0.0F) *
                          glm::scale(glm::mat4(1.0F), coin.scale);
    }
}

glm::vec3 ring_position(std::int32_t index, std::int32_t count, float radius) {
    const float angle = count > 0 ? static_cast<float>(index) / static_cast<float>(count) *
                                        glm::two_pi<float>()
                                  : 0.0F;
    return {std::cos(angle) * radius, 1.0F, std::sin(angle) * radius};
}

std::expected<void, engine::core::Error> register_module(engine::game::Registrar& registrar) {
    registrar.component<CoinSpin>(
        "coin_spin", "Spins and bobs a coin (Coin Collector)",
        engine::game::ComponentFields<CoinSpin>()
            .field("spin_speed", &CoinSpin::spin_speed,
                   {.min = -3600.0, .max = 3600.0, .description = "Spin, degrees per second"})
            .field("bob_height", &CoinSpin::bob_height,
                   {.min = 0.0, .max = 10.0, .description = "Bob amplitude, metres"})
            .field("bob_rate", &CoinSpin::bob_rate,
                   {.min = 0.0, .max = 100.0, .description = "Bob speed, radians per second"})
            .field("anchored", &CoinSpin::anchored,
                   {.description = "The rest pose has been captured", .read_only = true}));

    registrar.system({.name = "coin_collector.spin",
                      .phase = engine::game::Phase::pre_physics,
                      .run = &spin_coins});

    registrar.method({.name = "coins.status",
                      .summary = "Coins left in the scene: {} -> {remaining}.",
                      .params_schema = {{"type", "object"},
                                        {"properties", json::object()},
                                        {"additionalProperties", false}},
                      .result_schema = {{"type", "object"},
                                        {"properties", {{"remaining", {{"type", "integer"}}}}}},
                      .handler = [](engine::game::SystemContext& context, const json&)
                          -> std::expected<json, engine::core::Error> {
                          return json{{"remaining", context.registry().view<CoinSpin>().size()}};
                      }});

    registrar.lua_module(
        {.name = "coin_collector",
         .functions = {{.name = "ring_position",
                        .fn = [](const json& args) -> std::expected<json, engine::core::Error> {
                            // Script arguments are untrusted: checked reads only.
                            std::int64_t index = 0;
                            std::int64_t count = 0;
                            float        radius = 0.0F;
                            const bool ok = args.size() == 3 && JsonFields::read_int(args[0], index) &&
                                            JsonFields::read_int(args[1], count) &&
                                            JsonFields::read_float(args[2], radius) && index >= 0 &&
                                            count > 0 && count <= 100'000 && index < count;
                            if (!ok) {
                                return std::unexpected(engine::core::Error{
                                    "expected (index, count, radius) with 0 <= index < count <= 100000"});
                            }
                            const glm::vec3 p = ring_position(static_cast<std::int32_t>(index),
                                                              static_cast<std::int32_t>(count), radius);
                            return json::array({p.x, p.y, p.z});
                        }}}});
    return {};
}

} // namespace coin_collector
