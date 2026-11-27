#pragma once

// World-building component editors for the Inspector (ROADMAP E8.3).
// Private editor header; each draws property-grid rows (call inside
// ui::begin_property_grid).

#include <filesystem>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

namespace engine::scene {
struct Terrain;
struct Primitive;
struct InstancedMesh;
struct Scatter;
} // namespace engine::scene

namespace engine::editor {

void draw_terrain(scene::Terrain& terrain);
void draw_primitive(scene::Primitive& primitive);
// `project_root` confines the model path picker.
void draw_instanced_mesh(entt::registry& registry, entt::entity entity, scene::InstancedMesh& mesh,
                         const std::filesystem::path& project_root);
void draw_scatter(entt::registry& registry, entt::entity entity, scene::Scatter& scatter);

} // namespace engine::editor
