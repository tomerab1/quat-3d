#include "engine/scene/primitives.hpp"

#include <cmath>

#include <glm/gtc/constants.hpp>

namespace engine::scene {

// A unit cube centred at the origin with per-face normals (24 vertices, 36
// indices). Stands in as the demo mesh until glTF scene-graph loading (4.4).
MeshData make_cube_data() {
    using asset::Vertex;
    engine::scene::MeshData d;
    const glm::vec3 face_normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const glm::vec3& n : face_normals) {
        const glm::vec3 up = (std::abs(n.y) > 0.9F) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 t = glm::normalize(glm::cross(up, n));
        const glm::vec3 b = glm::cross(n, t);
        const glm::vec3 c = n * 0.5F;
        const glm::vec3 corners[4] = {
            c - t * 0.5F - b * 0.5F, c + t * 0.5F - b * 0.5F,
            c + t * 0.5F + b * 0.5F, c - t * 0.5F + b * 0.5F};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const auto base = static_cast<std::uint32_t>(d.vertices.size());
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.position = corners[i];
            v.normal = n;
            v.uv = uvs[i];
            v.tangent = glm::vec4(t, 1.0F);
            d.vertices.push_back(v);
        }
        for (std::uint32_t idx : {base, base + 1, base + 2, base, base + 2, base + 3}) {
            d.indices.push_back(idx);
        }
    }
    d.bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    d.submeshes = {asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
    return d;
}

// A box with the given half-extents (the unit cube scaled into geometry, so the
// mesh carries no Transform scale — keeps physics rotation extraction clean).
MeshData make_box_data(const glm::vec3& half_extents) {
    engine::scene::MeshData d = make_cube_data();
    const glm::vec3 s = half_extents / 0.5F;
    for (asset::Vertex& v : d.vertices) {
        v.position *= s;
    }
    d.bounds = {-half_extents, half_extents};
    return d;
}

// A unit-radius UV sphere (latitude/longitude grid). Good for showing off PBR
// metallic/roughness variation in the showcase scene.
MeshData make_sphere_data(float radius, std::uint32_t rings,
                                         std::uint32_t sectors) {
    using asset::Vertex;
    engine::scene::MeshData d;
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>(); // 0..pi (pole to pole)
        const float y = std::cos(phi);
        const float ring_r = std::sin(phi);
        for (std::uint32_t s = 0; s <= sectors; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(sectors);
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 n{ring_r * std::cos(theta), y, ring_r * std::sin(theta)};
            Vertex vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = {u, v};
            // Tangent along +theta (east).
            vert.tangent = glm::vec4(-std::sin(theta), 0.0F, std::cos(theta), 1.0F);
            d.vertices.push_back(vert);
        }
    }
    const std::uint32_t stride = sectors + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < sectors; ++s) {
            const std::uint32_t i0 = r * stride + s;
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + stride;
            const std::uint32_t i3 = i2 + 1;
            for (std::uint32_t idx : {i0, i2, i1, i1, i2, i3}) d.indices.push_back(idx);
        }
    }
    d.bounds = {glm::vec3(-radius), glm::vec3(radius)};
    d.submeshes = {asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
    return d;
}

} // namespace engine::scene
