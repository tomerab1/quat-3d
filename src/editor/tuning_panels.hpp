#pragma once

// Look-tuning UI (ROADMAP A8). Private editor header. Both panels draw from
// the same descriptor tables the remote API and the codecs use
// (render_setting_fields(), material_fields()), so the editor shows exactly
// the values and ranges an AI assistant sees through render.settings.* and
// material.*.

#include <entt/entity/fwd.hpp>

namespace engine::renderer {
struct RenderSettings;
}
namespace engine::scene {
class Scene;
}

namespace engine::editor {

// The "Renderer" window: the scene's sun (azimuth / elevation / colour /
// intensity of its DirectionalLight), then one section per RenderSettings
// group with a reset button.
void draw_renderer_panel(renderer::RenderSettings& settings, scene::Scene* scene);

// The Inspector's "Material" section for an entity drawn with a material
// (primitive or mesh): every factor of material_fields(), editable; a factor
// overridden on this entity gets a revert button.
void draw_material_inspector(entt::registry& registry, entt::entity entity);

} // namespace engine::editor
