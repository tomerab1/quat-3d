#pragma once

// Inspector editor for a Primitive's shape and material (Phase 15) including
// its authored material shader selection (ROADMAP A6). Private editor header;
// the Primitive and InstancedMesh inspectors both delegate here.

namespace engine::scene {
struct Primitive;
}

namespace engine::editor {

// Shape, colour, metallic, roughness, emission, material shader name (applied
// on Enter; empty = built-in) and the shader's two parameter vectors.
void draw_primitive_fields(scene::Primitive& primitive);

} // namespace engine::editor
