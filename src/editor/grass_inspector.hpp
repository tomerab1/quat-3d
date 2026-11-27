#pragma once

// Inspector editor for the Grass component (R9.3). Private editor header;
// inspector.cpp's ComponentInspector specialisation delegates here so the
// inspector file stays within the size rule.

namespace engine::scene {
struct Grass;
} // namespace engine::scene

namespace engine::editor {

// Density / radius, blade shape, colours (edited in sRGB, stored linear),
// wind, slope / height limits, ground and LOD fields.
void draw_grass(scene::Grass& grass);

} // namespace engine::editor
