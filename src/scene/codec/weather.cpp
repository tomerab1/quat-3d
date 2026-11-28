// Codec component `weather`: The scene's weather (one per scene): wind, clouds, rain/snow, wetness, snow cover, temperature.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/weather_json.hpp"
#include "engine/weather/weather_state.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;

} // namespace

std::expected<void, core::Error> register_weather(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "weather",
                         .summary = "The scene's weather (one per scene): wind, clouds, rain/snow, wetness, snow cover, temperature",
                         .addable = true,
                         .writable = true,
                         .order = 1900};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const weather::WeatherState& w = r.get<weather::WeatherState>(e);
        return json(encode_weather(w));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        // One weather per scene: a second entity cannot take the component.
        if (const entt::entity owner = weather::find_weather(r); owner != entt::null && owner != e) {
            return std::unexpected(core::Error{
                "weather: the scene already has its weather on entity " +
                std::to_string(entt::to_integral(owner)) + " (edit that one, or remove it first)"});
        }
        weather::WeatherState w =
            r.all_of<weather::WeatherState>(e) ? r.get<weather::WeatherState>(e) : weather::WeatherState{};
        if (auto ok = apply_weather_json(w, v, strict, "weather"); !ok) return ok;
        r.emplace_or_replace<weather::WeatherState>(e, w);
        return {};
    };
    return registry.add(typed_codec<weather::WeatherState>(std::move(codec)));
}

} // namespace engine::scene::codec
