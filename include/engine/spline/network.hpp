#pragma once

// Curve networks (ROADMAP G18.2): many curves joined into a planar graph seen
// from above (XZ). Endpoints within a snap radius join one node; an endpoint
// on another curve's interior splits it there (a T); curves split where they
// cross (an X). Nodes are classified by degree and angle, and nodes and edges
// get ids derived from the source curve ids, so editing one curve keeps the
// ids of everything it does not touch (incremental regeneration, undo).
// Generic: walls, roads, rivers, rails and fences all build on it.

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/spline/curve.hpp"

namespace engine::spline {

struct NetworkCurve {
    std::uint32_t id = 0;          // unique within the network; ids derive from it
    const Curve*  curve = nullptr; // world space
};

struct NetworkOptions {
    // Endpoints this close (XZ, metres) join one node; an endpoint this close
    // to another curve's interior splits it there.
    float snap_radius = 0.25F;
    // A degree-2 node is `straight` when its two edges leave within this
    // cosine of exactly opposite (cos 15 deg = 0.9659), else a `bend`.
    float straight_cos = 0.9659258F;
    // Crossing search tolerance (metres).
    float crossing_tolerance = 1.0e-3F;
};

enum class NodeKind : std::uint8_t {
    end,      // degree 1: a dead end
    straight, // degree 2, continuing straight on
    bend,     // degree 2, turning (a corner)
    tee,      // degree 3
    cross,    // degree 4
    n_way,    // degree 5+
};
[[nodiscard]] std::string_view node_kind_name(NodeKind kind);

// One edge leaving a node.
struct NodeEnd {
    std::uint32_t edge = 0;          // index into CurveNetwork::edges
    bool          at_start = true;   // the edge's t0 end (else its t1 end)
    glm::vec2     direction{1.0F, 0.0F}; // unit XZ direction leaving the node
};

struct NetworkNode {
    std::uint64_t        id = 0;
    glm::vec3            position{0.0F}; // mean of the joined points
    NodeKind             kind = NodeKind::end;
    std::vector<NodeEnd> ends;           // counter-clockwise seen from above (+Y), from +X
};

struct NetworkEdge {
    std::uint64_t id = 0;
    std::uint32_t curve_id = 0;
    std::uint32_t curve_index = 0; // into the input span
    // Parameter range on the source curve. On a closed curve t1 may exceed
    // its max_param: the edge runs over the seam (Curve wraps parameters).
    float         t0 = 0.0F;
    float         t1 = 0.0F;
    float         length = 0.0F;
    std::uint32_t node_a = 0; // at t0 (index into nodes)
    std::uint32_t node_b = 0; // at t1
};

struct CurveNetwork {
    std::vector<NetworkNode> nodes; // sorted by id
    std::vector<NetworkEdge> edges; // sorted by (curve id, t0)

    [[nodiscard]] const NetworkNode* find_node(std::uint64_t id) const;
    [[nodiscard]] const NetworkEdge* find_edge(std::uint64_t id) const;
};

// Builds the network. Errors: a null or empty curve, a duplicated id, a
// non-positive snap radius. Deterministic for a given input order-free set:
// the result does not depend on the order of `curves`.
[[nodiscard]] std::expected<CurveNetwork, core::Error> build_network(std::span<const NetworkCurve> curves,
                                                                     const NetworkOptions& options = {});

} // namespace engine::spline
