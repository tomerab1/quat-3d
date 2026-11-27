#pragma once

// Flat-shaded, low-poly mesh construction for the offline nature-asset
// generator (tools/asset-gen). A Model is a set of materials and one triangle
// list per material; every triangle owns its three vertices and carries its
// face normal (the faceted, stylised look). Everything is deterministic: the
// random source is a seeded PCG32 and the noise is a hash-based value noise,
// so the same seed always produces the same geometry on a given platform.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace quat::asset_gen {

// glTF metallic-roughness factors (linear base colour).
struct Material {
    std::string name;
    glm::vec4   base_color{1.0F};
    float       metallic = 0.0F;
    float       roughness = 0.9F;
};

// One glTF primitive: a triangle list drawn with `material`.
struct Primitive {
    std::uint32_t              material = 0;
    std::vector<glm::vec3>     positions;
    std::vector<glm::vec3>     normals;
    std::vector<std::uint32_t> indices;

    [[nodiscard]] std::size_t triangle_count() const { return indices.size() / 3; }
};

struct Model {
    std::string            name;
    std::vector<Material>  materials;
    std::vector<Primitive> primitives;

    // The primitive drawing `material` (created on first use).
    Primitive& part(std::uint32_t material);
    [[nodiscard]] std::size_t triangle_count() const;
    // Axis-aligned bounds of every vertex.
    void bounds(glm::vec3& lo, glm::vec3& hi) const;
};

// PCG32 (O'Neill), with an explicit float mapping so results never depend on
// the standard library's distributions.
class Rng {
public:
    explicit Rng(std::uint64_t seed, std::uint64_t stream = 0x5851F42D4C957F2DULL);
    std::uint32_t next();
    // Uniform in [lo, hi).
    float uniform(float lo, float hi);

private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 0;
};

// Smooth value noise in [0, 1] over integer lattice cells.
[[nodiscard]] float value_noise(const glm::vec3& p, std::uint32_t seed);

// Appends a triangle (counter-clockwise seen from outside) with its face
// normal. Degenerate triangles are skipped.
void add_triangle(Primitive& prim, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

// A closed ring of points around `axis` through `centre`.
struct Ring {
    glm::vec3              centre{0.0F};
    std::vector<glm::vec3> points;
};

// `sides` points on a circle of `radius` in the plane through `centre`
// perpendicular to `axis`, starting at angle `phase` (radians).
[[nodiscard]] Ring make_ring(const glm::vec3& centre, const glm::vec3& axis, float radius,
                             int sides, float phase);

// The band of quads between two rings with the same point count.
void add_band(Primitive& prim, const Ring& lower, const Ring& upper);
// A fan from `ring` to `apex`; `outward` picks the winding for a surface
// facing away from the ring's centre (a cone's side) or toward it (a cap
// seen from below).
void add_fan(Primitive& prim, const Ring& ring, const glm::vec3& apex, bool outward);

// A tapered, closed cylinder from `base` to `top`.
void add_frustum(Primitive& prim, const glm::vec3& base, const glm::vec3& top, float base_radius,
                 float top_radius, int sides, float phase);

// A noised icosphere ("blob"): `subdivisions` (0..3) splits of an
// icosahedron, scaled by `radii`, each vertex pushed along its direction by
// up to `roughness` (fraction of the radius) of value noise sampled at
// `frequency` per unit direction.
struct BlobParams {
    glm::vec3     centre{0.0F};
    glm::vec3     radii{1.0F};
    int           subdivisions = 1;
    float         roughness = 0.2F;
    float         frequency = 2.0F;
    std::uint32_t seed = 1;
    float         floor_y = -1.0e9F; // vertices below are clamped up to it (flat bases)
};
void add_blob(Primitive& prim, const BlobParams& params);

} // namespace quat::asset_gen
