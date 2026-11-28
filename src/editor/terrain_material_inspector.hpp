#pragma once

// Inspector rows of the layered terrain material (ROADMAP R26.1). Private
// editor header: call inside ui::begin_property_grid.

namespace engine::terrain {
struct TerrainMaterial;
}

namespace engine::editor {

void draw_terrain_material(terrain::TerrainMaterial& material);

} // namespace engine::editor
