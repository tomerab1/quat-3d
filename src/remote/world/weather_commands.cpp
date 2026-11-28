// weather.get / weather.set (ROADMAP R13.1): the scene's weather singleton.

#include <string>

#include <nlohmann/json.hpp>

#include "engine/scene/scene.hpp"
#include "engine/scene/weather_json.hpp"
#include "engine/weather/weather_state.hpp"
#include "engine/weather/wind_field.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_wind_points = 1024;

// The scene's weather, or the defaults the renderer uses when it has none.
[[nodiscard]] weather::WeatherState current(const entt::registry& r, entt::entity e) {
    return e != entt::null ? r.get<weather::WeatherState>(e) : weather::WeatherState{};
}

RpcResult weather_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    bool schema = false;
    f.boolean("schema", schema);
    const Json* points = f.raw("wind_at");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    std::vector<glm::vec3> query;
    if (points != nullptr && !points->is_null()) {
        if (!points->is_array() || points->size() > k_max_wind_points) {
            return bad_params("wind_at: expected an array of at most " +
                              std::to_string(k_max_wind_points) + " [x, y, z] points");
        }
        for (std::size_t i = 0; i < points->size(); ++i) {
            glm::vec3 p{};
            if (!core::JsonFields::read_vec3((*points)[i], p)) {
                return bad_params("wind_at[" + std::to_string(i) +
                                  "]: expected [x, y, z] (finite numbers; y = height above the "
                                  "ground in metres)");
            }
            query.push_back(p);
        }
    }

    const entt::registry& r = ctx.scene->registry();
    const entt::entity e = weather::find_weather(r);
    const weather::WeatherState state = current(r, e);
    Json out = {{"present", e != entt::null},
                {"entity", e != entt::null ? entity_json(e) : Json(nullptr)},
                {"weather", scene::encode_weather(state)}};
    if (!query.empty()) {
        // The renderer's clock when the application has one, so the answer
        // matches what the grass shows this frame.
        const weather::WindAdvection clock =
            ctx.services.wind_clock ? ctx.services.wind_clock() : weather::WindAdvection{};
        const weather::WindFieldParams field = weather::wind_field_params(state, clock);
        Json winds = Json::array();
        for (const glm::vec3& p : query) {
            const weather::WindSample s = weather::sample_wind(field, {p.x, p.z});
            winds.push_back(vec3_json(s.velocity * weather::wind_profile(p.y)));
        }
        out["wind_at"] = std::move(winds);
    }
    if (schema) out["schema"] = scene::weather_schema()["fields"];
    return out;
}

RpcResult weather_set(Context& ctx, const Json& params) {
    if (!params.is_object() && !params.is_null()) {
        return bad_params("expected an object of weather fields");
    }
    entt::registry& r = ctx.scene->registry();
    const entt::entity existing = weather::find_weather(r);

    Json fields = params.is_null() ? Json::object() : params;
    bool reset = false;
    if (const auto it = fields.find("reset"); it != fields.end()) {
        if (!it->is_boolean()) return bad_params("reset: expected true or false");
        reset = it->get<bool>();
        fields.erase(it);
    }
    // Decode completely before changing anything.
    weather::WeatherState state = reset ? weather::WeatherState{} : current(r, existing);
    if (auto ok = scene::apply_weather_json(state, fields, /*strict=*/true, "weather"); !ok) {
        return bad_params(ok.error().message);
    }
    const entt::entity e = existing != entt::null ? existing : ctx.scene->create_entity("Weather");
    r.emplace_or_replace<weather::WeatherState>(e, state);
    return weather_get(ctx, Json::object());
}

} // namespace

void register_weather_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("weather.get",
                      "The scene's weather (defaults when it has none: present = false). "
                      "{schema: true} adds the field table; {wind_at: [[x, y, z], ...]} "
                      "(<= 1024; y = height above the ground) adds the wind velocity there.",
                      [ctx](const Json& p) { return weather_get(*ctx, p); });
    server.add_method("weather.set",
                      "Merge weather fields (see weather.get schema), creating the scene's "
                      "weather entity if needed; {reset: true} starts from the defaults. "
                      "Returns weather.get.",
                      [ctx](const Json& p) { return weather_set(*ctx, p); });
}

} // namespace engine::remote::world
