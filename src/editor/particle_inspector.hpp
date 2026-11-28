#pragma once

// Inspector editor for the ParticleEmitter component (R10.3). Private editor
// header; inspector.cpp's ComponentInspector specialisation delegates here so
// the inspector file stays within the size rule.

namespace engine::scene {
struct ParticleEmitter;
} // namespace engine::scene

namespace engine::editor {

// Emission, shape, forces, size / colour curves (colours edited in sRGB,
// stored linear), flipbook, rendering and collision fields.
void draw_particle_emitter(scene::ParticleEmitter& emitter);

} // namespace engine::editor
