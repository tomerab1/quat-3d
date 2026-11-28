#include "spinner.hpp"

#include <cmath>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace consumer {

void spinner_system(entt::registry& registry, float dt) {
    auto view = registry.view<Spinner, engine::scene::Transform>();
    for (const entt::entity e : view) {
        Spinner& spin = view.get<Spinner>(e);
        spin.angle_degrees = std::fmod(spin.angle_degrees + spin.degrees_per_second * dt, 360.0F);
        if (spin.angle_degrees < 0.0F) spin.angle_degrees += 360.0F;
        glm::mat4& local = view.get<engine::scene::Transform>(e).local;
        const glm::vec4 translation = local[3];
        local = glm::rotate(glm::mat4(1.0F), glm::radians(spin.angle_degrees),
                            glm::vec3(0.0F, 1.0F, 0.0F));
        local[3] = translation;
    }
}

void register_spinner_methods(engine::remote::RpcServer& server, engine::scene::Scene& scene) {
    server.add_method("consumer.spinners", "List the scene's spinners and their angles",
                      [&scene](const engine::remote::Json& params) -> engine::remote::RpcResult {
                          if (!params.is_null() && !(params.is_object() && params.empty())) {
                              return engine::remote::rpc_fail(engine::remote::rpc_code::invalid_params,
                                                              "consumer.spinners takes no parameters");
                          }
                          engine::remote::Json angles = engine::remote::Json::array();
                          for (const entt::entity e : scene.registry().view<Spinner>()) {
                              angles.push_back(scene.registry().get<Spinner>(e).angle_degrees);
                          }
                          return engine::remote::Json{{"count", angles.size()}, {"angles", angles}};
                      });
}

nlohmann::json save_spinners(const entt::registry& registry) {
    nlohmann::json entries = nlohmann::json::object();
    for (const entt::entity e : registry.view<Spinner, engine::scene::Name>()) {
        const Spinner& spin = registry.get<Spinner>(e);
        entries[registry.get<engine::scene::Name>(e).value] = {
            {"degrees_per_second", spin.degrees_per_second}, {"angle_degrees", spin.angle_degrees}};
    }
    return nlohmann::json{{"version", 1}, {"spinners", entries}};
}

std::expected<std::size_t, engine::core::Error> load_spinners(entt::registry& registry,
                                                              const nlohmann::json& chunk) {
    engine::core::JsonFields root(chunk, "spinners chunk");
    if (!root.has("version")) {
        return std::unexpected(engine::core::make_error("spinners chunk: \"version\" is required"));
    }
    int version = 0;
    root.integer("version", version, 1, 1);
    const nlohmann::json* entries = root.raw("spinners");
    if (auto done = root.finish(); !done) return std::unexpected(done.error());
    if (entries == nullptr || !entries->is_object()) {
        return std::unexpected(engine::core::make_error("spinners chunk: \"spinners\" must be an object"));
    }
    std::size_t restored = 0;
    for (const auto& [name, value] : entries->items()) {
        Spinner spin;
        engine::core::JsonFields fields(value, "spinners chunk: " + name);
        fields.number("degrees_per_second", spin.degrees_per_second, -3600.0F, 3600.0F)
            .number("angle_degrees", spin.angle_degrees, 0.0F, 360.0F);
        if (auto done = fields.finish(); !done) return std::unexpected(done.error());
        for (const entt::entity e : registry.view<engine::scene::Name>()) {
            if (registry.get<engine::scene::Name>(e).value == name) {
                registry.emplace_or_replace<Spinner>(e, spin);
                ++restored;
            }
        }
    }
    return restored;
}

} // namespace consumer
