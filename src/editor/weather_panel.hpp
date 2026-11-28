#pragma once

// The Renderer panel's "Weather" section (ROADMAP R13.1). Private editor
// header. Widgets come from the weather field table
// (engine/weather/weather_state.hpp), so the editor shows exactly the fields,
// ranges and hints the codec, weather.* and the quat-mcp tools use.

namespace engine::scene {
class Scene;
}

namespace engine::editor {

// Edits the scene's weather singleton in place; without one, shows that the
// defaults apply and offers to add it.
void draw_weather_section(scene::Scene* scene);

} // namespace engine::editor
