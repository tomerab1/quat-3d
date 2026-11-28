#include "component_json_dynamic_mesh.hpp"

#include "component_json_internal.hpp"

namespace engine::scene::detail {

namespace {

constexpr core::EnumName<DynamicCollider> k_dynamic_colliders[] = {
    {"none", DynamicCollider::none},
    {"mesh", DynamicCollider::mesh},
};

// Generator specs are checked when they run (geometry_spec.cpp); here only
// their size is bounded.
constexpr std::size_t k_max_generator_bytes = std::size_t{1} << 20;

} // namespace

nlohmann::json encode_dynamic_mesh(const DynamicMesh& m) {
    nlohmann::json j = {{"mesh", m.mesh},
                        {"material", primitive_material_json(m.material)},
                        {"collider", m.collider == DynamicCollider::mesh ? "mesh" : "none"}};
    if (!m.generator.is_null()) {
        j["generator"] = m.generator;
    }
    return j;
}

void decode_dynamic_mesh(core::JsonFields& f, DynamicMesh& m) {
    const std::string before = m.mesh;
    f.string("mesh", m.mesh, 128);
    if (m.mesh != before && !valid_dynamic_mesh_id(m.mesh)) {
        f.fail("mesh", "expected 1-128 printable ASCII characters without spaces");
    }
    if (const nlohmann::json* g = f.raw("generator")) {
        if (g->is_null()) {
            m.generator = nullptr;
        } else if (!g->is_object()) {
            f.fail("generator", "expected a generator spec object (see docs/geometry.md) or null");
        } else if (g->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size() > k_max_generator_bytes) {
            f.fail("generator", "is larger than 1 MiB");
        } else {
            m.generator = *g;
        }
    }
    if (const nlohmann::json* mat = f.raw("material")) {
        core::JsonFields mf(*mat, "dynamic_mesh.material", f.strict());
        read_primitive_material(mf, m.material);
        if (auto done = mf.finish(); !done) {
            f.fail("material", done.error().message); // names the nested field
        }
    }
    f.enumeration("collider", m.collider, k_dynamic_colliders);
    // Rebuild the MeshRenderer and the body from the (possibly new) mesh.
    m.resolved_version = 0;
    m.resolved_material = 0;
    m.collider_version = 0;
}

} // namespace engine::scene::detail
