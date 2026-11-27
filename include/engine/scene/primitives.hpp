#pragma once

// Procedural mesh generators (CPU-side MeshData, upload with upload_mesh()).
// Used by the demo scenes, the tests, and the Primitive component.

#include <cstdint>

#include <glm/glm.hpp>

#include "engine/scene/gltf_loader.hpp" // MeshData

namespace engine::scene {

// Unit cube centred at the origin, per-face normals (24 vertices, 36 indices).
[[nodiscard]] MeshData make_cube_data();

// Box with the given half-extents baked into the geometry (no Transform scale
// needed — keeps physics rotation extraction clean).
[[nodiscard]] MeshData make_box_data(const glm::vec3& half_extents);

// UV sphere (latitude/longitude grid) of `radius`.
[[nodiscard]] MeshData make_sphere_data(float radius, std::uint32_t rings, std::uint32_t sectors);

} // namespace engine::scene
