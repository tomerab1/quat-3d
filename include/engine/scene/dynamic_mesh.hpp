#pragma once

// Runtime-generated meshes (ROADMAP R23.2; design: docs/design/cozy-builder.md
// §4.4, user guide: docs/geometry.md).
//
// A game (or a remote tool, or a script) submits CPU geometry::MeshData to the
// DynamicMeshStore under a string id; entities show it with a DynamicMesh
// component naming that id. The store uploads new versions in the background
// through a staging ring with a per-frame byte budget; the previous version
// keeps drawing until the new one is resident, then the swap happens at a
// frame boundary and the old buffers are freed once no frame in flight uses
// them. Resolved DynamicMeshes get an ordinary MeshRenderer, so batching,
// shadows, motion vectors, culling, bounds and picking treat them like any
// other mesh.
//
// This header is graphics-API free (SDK stable set); the GPU side lives in
// the renderer and the frame loop.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/asset/mesh_types.hpp"
#include "engine/core/error.hpp"
#include "engine/geometry/mesh_data.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

// Collision built from the mesh (static bodies only, CLAUDE.md
// collider_mesh). The body's owner is the entity, so ray casts map back.
enum class DynamicCollider : std::uint8_t { none, mesh };

struct DynamicMesh {
    std::string mesh;          // id in the DynamicMeshStore
    // Optional generator spec (scene/geometry_spec.hpp). When set and the
    // store has no mesh under `mesh`, the spec is run and its output
    // submitted, so scene files regenerate what they show. Editing it
    // regenerates the mesh.
    nlohmann::json generator;
    Primitive material{};      // flat material: colour, metallic, roughness, emissive, shader
    DynamicCollider collider = DynamicCollider::none;
    // Runtime state (not serialised): what the MeshRenderer / body were built
    // from, so edits and new versions are picked up.
    std::uint64_t resolved_version = 0;
    std::uint64_t resolved_material = 0;
    std::uint64_t generated_hash = 0;
    std::uint64_t collider_version = 0;
    std::uint32_t body = 0xFFFFFFFFU; // physics body id, or none
};

// What the store knows about one mesh.
struct DynamicMeshInfo {
    std::string id;
    std::uint64_t version = 0;          // latest submitted
    std::uint64_t resident_version = 0; // drawn now (0: nothing yet)
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    std::size_t bytes = 0;              // GPU bytes of the latest version
    asset::Aabb bounds{};
    std::uint64_t hash = 0;             // geometry::mesh_hash of the latest version
};

struct DynamicMeshStats {
    std::size_t meshes = 0;
    std::size_t pending_uploads = 0;
    std::size_t pending_bytes = 0;
    std::size_t uploaded_bytes_last_frame = 0;
    std::size_t resident_bytes = 0;
};

class DynamicMeshStore {
public:
    struct Config {
        // Bytes copied to the GPU per frame at most (the staging ring holds
        // this much per frame in flight). Larger meshes upload over several
        // frames.
        std::size_t frame_byte_budget = std::size_t{8} << 20;
        std::uint32_t frames_in_flight = 2;
        std::size_t max_mesh_bytes = std::size_t{256} << 20;
    };

    DynamicMeshStore();
    explicit DynamicMeshStore(const Config& config);
    DynamicMeshStore(DynamicMeshStore&&) noexcept;
    DynamicMeshStore& operator=(DynamicMeshStore&&) noexcept;
    ~DynamicMeshStore();

    // Stores `mesh` under `id` (validated: geometry::validate) and queues its
    // upload. Returns the new version (1, 2, ...). Ids are 1-128 printable
    // ASCII characters.
    [[nodiscard]] std::expected<std::uint64_t, core::Error> submit(std::string_view id, geometry::MeshData mesh);

    // Replaces vertices [first, first + vertices.size()) of the latest
    // version, keeping the topology. Only the changed byte range is staged;
    // the rest is copied on the GPU from the previous version when that is
    // resident (else the whole mesh is uploaded).
    [[nodiscard]] std::expected<std::uint64_t, core::Error>
    update_vertices(std::string_view id, std::uint32_t first, std::span<const asset::Vertex> vertices);

    bool remove(std::string_view id);
    void clear();

    [[nodiscard]] bool contains(std::string_view id) const;
    [[nodiscard]] const geometry::MeshData* mesh(std::string_view id) const; // latest version
    [[nodiscard]] std::optional<DynamicMeshInfo> info(std::string_view id) const;
    [[nodiscard]] std::vector<DynamicMeshInfo> list() const;
    [[nodiscard]] DynamicMeshStats stats() const;
    [[nodiscard]] const Config& config() const;

    // Implementation (GPU slots, upload queue): see src/scene/dynamic_mesh_store_impl.hpp.
    struct Impl;
    [[nodiscard]] Impl& impl() { return *impl_; }
    [[nodiscard]] const Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// True for a valid store id.
[[nodiscard]] bool valid_dynamic_mesh_id(std::string_view id);

} // namespace engine::scene
