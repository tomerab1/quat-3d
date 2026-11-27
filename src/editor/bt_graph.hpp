#pragma once

// Behaviour-tree graph model for the visual editor (ROADMAP G2). Private
// editor header; no ImGui — the Behaviour panel draws and edits a BtGraph, and
// this layer converts it to and from the tree JSON the runtime compiles
// (BehaviorTree::source, see engine/scene/behavior.hpp).
//
// The graph is a flat list of nodes with stable ids; a link is a parent's
// entry in `children` (ordered: the runtime ticks children in that order).
// While editing, the graph may be temporarily invalid (a composite without
// children, a node not yet connected); bt_graph_validate lists what is wrong
// per node, and only a valid graph serialises.
//
// Layout lives in the root object's optional "editor" field, which the
// runtime ignores:
//
//     "editor": {"version": 1, "nodes": [[id, x, y], ...]}
//
// one entry per node in depth-first pre-order (the order the runtime stores
// compiled nodes in). Loading reuses those ids and positions when the entry
// count matches the tree; a tree edited by hand into a different shape gets
// fresh ids and an automatic layout instead.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/behavior.hpp"

namespace engine::editor {

using BtKind = scene::BehaviorTreeAsset::Kind;

// A leaf parameter present in the JSON (absent ones take the leaf's default).
// Nodes list them in the leaf's declared order, unknown names last.
struct BtGraphParam {
    std::string name;
    float       value = 0.0F;
};

struct BtGraphNode {
    std::uint32_t              id = 0; // stable, never 0
    BtKind                     kind = BtKind::leaf;
    std::string                leaf;     // leaf type name (leaf nodes only)
    std::vector<BtGraphParam>  params;   // leaf nodes only
    std::vector<std::uint32_t> children; // ordered child ids (composites only)
    glm::vec2                  pos{0.0F}; // editor canvas position (top-left)
};

// A validation problem; `node` is 0 for graph-level issues.
struct BtGraphIssue {
    std::uint32_t node = 0;
    std::string   message;
};

struct BtGraph {
    std::vector<BtGraphNode> nodes;
    std::uint32_t            next_id = 1;

    [[nodiscard]] BtGraphNode*       find(std::uint32_t id);
    [[nodiscard]] const BtGraphNode* find(std::uint32_t id) const;
};

// Leaf catalogue used for validation and new nodes (defaults to the built-in
// leaves' reflection table).
using BtLeafCatalog = std::span<const scene::BtLeafInfo>;

// --- JSON <-> graph --------------------------------------------------------

// Builds a graph from tree JSON. Fails only on JSON the graph cannot represent
// (malformed JSON, unknown node types or fields, non-numeric parameters);
// semantic problems (unknown leaf, out-of-range value, wrong child count) are
// kept so the editor can show and fix them — see bt_graph_validate.
[[nodiscard]] std::expected<BtGraph, core::Error>
bt_graph_from_json(std::string_view json_text, BtLeafCatalog leaves = scene::bt_builtin_leaf_info());

// Serialises a valid graph (pretty-printed tree + one-line "editor" layout).
// Fails with the first validation issue otherwise.
[[nodiscard]] std::expected<std::string, core::Error>
bt_graph_to_json(const BtGraph& graph, BtLeafCatalog leaves = scene::bt_builtin_leaf_info());

// Everything that keeps the graph from being a runtime-valid tree: exactly
// one root, one parent per node, no cycles, every node connected, child
// counts (composites >= 1, inverter exactly 1, leaves none), known leaves,
// known and in-range parameters, nesting depth.
[[nodiscard]] std::vector<BtGraphIssue>
bt_graph_validate(const BtGraph& graph, BtLeafCatalog leaves = scene::bt_builtin_leaf_info());

// --- queries -----------------------------------------------------------------

// The single parentless node, or 0 when there is none or more than one.
[[nodiscard]] std::uint32_t bt_graph_root(const BtGraph& graph);
// The node listing `child` among its children (the first, if several), or 0.
[[nodiscard]] std::uint32_t bt_graph_parent(const BtGraph& graph, std::uint32_t child);
// Ids reachable from the root, depth-first pre-order (= the runtime's
// compiled node order for a valid graph). Empty without a single root; stops
// at nodes already visited, so cycles terminate.
[[nodiscard]] std::vector<std::uint32_t> bt_graph_preorder(const BtGraph& graph);

// --- edits (keep one parent per node and no cycles) --------------------------

// Adds a node; leaves get the catalogue's non-optional parameters at their
// defaults. Returns its id.
std::uint32_t bt_graph_add_node(BtGraph& graph, BtKind kind, std::string_view leaf,
                                glm::vec2 pos,
                                BtLeafCatalog leaves = scene::bt_builtin_leaf_info());

// Removes a node and its links; its children become parentless.
void bt_graph_remove_node(BtGraph& graph, std::uint32_t id);

// Whether `parent` -> `child` may be linked (a leaf cannot parent, no
// self-links or cycles, an inverter takes one child). An existing parent of
// `child` is not a reason to refuse: linking moves the child.
[[nodiscard]] std::expected<void, core::Error> bt_graph_can_link(const BtGraph& graph,
                                                                 std::uint32_t parent,
                                                                 std::uint32_t child);
// Links `child` as the last child of `parent` (detaching it from any previous
// parent), or reports why it cannot.
std::expected<void, core::Error> bt_graph_link(BtGraph& graph, std::uint32_t parent,
                                               std::uint32_t child);
// Removes the parent -> child link. Returns whether one existed.
bool bt_graph_unlink(BtGraph& graph, std::uint32_t parent, std::uint32_t child);

// Moves `child` one place earlier (delta < 0) or later (delta > 0) among its
// siblings, swapping canvas x with the sibling so the left-to-right order on
// screen keeps matching. Returns whether anything moved.
bool bt_graph_move_child(BtGraph& graph, std::uint32_t child, int delta);

// Re-orders every node's children by canvas x (left to right), the way the
// editor lets designers reorder by dragging. Returns whether any order changed.
bool bt_graph_sort_children_by_x(BtGraph& graph);

// Tidy top-down layout of the tree under the root (parents centred over their
// children); nodes outside the tree keep their positions.
void bt_graph_auto_layout(BtGraph& graph);

} // namespace engine::editor
