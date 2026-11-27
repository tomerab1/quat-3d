#pragma once

// Look-tuning UI (ROADMAP A8). Private editor header. Both panels draw from
// the same descriptor tables the remote API and the codecs use
// (render_setting_fields(), material_fields()), so the editor shows exactly
// the values and ranges an AI assistant sees through render.settings.* and
// material.*.

#include <entt/entity/fwd.hpp>

namespace engine::renderer {
struct PhysicalCamera;
struct RenderSettings;
}
namespace engine::scene {
class Scene;
}

namespace engine::editor {

// The "Renderer" window: the scene's sun (azimuth / elevation / colour /
// intensity of its DirectionalLight), the lens of the view camera (the
// editor's fly camera; null: none shown), then one section per
// RenderSettings group with a reset button.
void draw_renderer_panel(renderer::RenderSettings& settings, scene::Scene* scene,
                         renderer::PhysicalCamera* view_lens = nullptr, float view_fov_y = 1.0471976F);

// R20 physical lens controls (aperture, shutter, ISO, focal length, sensor,
// focus distance) plus the resulting EV100; `fov_y` is the camera's own
// field of view (used while the focal length is 0). Shared by the Camera
// inspector and the Renderer panel's view-camera section.
bool draw_lens_controls(renderer::PhysicalCamera& lens, float fov_y);

// The Inspector's "Material" section for an entity drawn with a material
// (primitive or mesh): every factor of material_fields(), editable — except a
// primitive's own colour / metallic / roughness / emissive, which belong to its
// Primitive component; a factor overridden on this entity gets a revert button.
void draw_material_inspector(entt::registry& registry, entt::entity entity);

} // namespace engine::editor
