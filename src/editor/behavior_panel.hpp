#pragma once

// "Behaviour" panel (ROADMAP G2): a visual node editor (imgui-node-editor) for
// the selected entity's BehaviorTree. Private editor header.
//
// The panel edits a BtGraph (engine/scene/bt_graph.hpp) mirrored from BehaviorTree::source:
// nodes are composites / decorators / leaves, a link runs from a parent's
// bottom pin to a child's top pin, and children tick left to right (dragging
// a node sideways reorders its siblings). Leaf parameters are edited inside
// the nodes, typed and ranged by the runtime's leaf reflection table. Every
// edit that leaves the graph valid is written back to `source` and recompiles
// the tree (the inspector's Recompile path); layout-only edits update the
// saved layout without recompiling, so moving nodes during Play keeps the
// tree's running state. While the graph is invalid, the issues are listed
// and `source` keeps the last valid tree. During Play the nodes the tree is
// running are highlighted.

#include <cstdint>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include "engine/scene/bt_graph.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}

namespace engine::scene {
class Scene;
struct BehaviorTree;
} // namespace engine::scene

namespace engine::editor {

inline constexpr const char* behavior_panel_title = "Behaviour";

using scene::BtGraph;
using scene::BtGraphIssue;
using scene::BtGraphNode;
using scene::BtGraphParam;
using scene::BtKind;

class BehaviorPanel {
public:
    // Creates the node-editor context; requires a live ImGui context. Destroy
    // the panel before the ImGui context.
    BehaviorPanel();
    ~BehaviorPanel();
    BehaviorPanel(const BehaviorPanel&) = delete;
    BehaviorPanel& operator=(const BehaviorPanel&) = delete;

    // Draws the window for `selected` (any entity; the panel explains what to
    // do when it has no BehaviorTree).
    void draw(scene::Scene& scene, entt::entity selected);

private:
    // How the graph changed this frame, in increasing order of consequence.
    enum class Change : std::uint8_t { none, layout, structure };

    void load(entt::entity entity, const scene::BehaviorTree& bt);
    void draw_toolbar(scene::BehaviorTree& bt);
    void draw_canvas(const scene::BehaviorTree& bt, float height);
    // `order` is the node's 1-based place among its siblings (0: only child).
    void draw_node(BtGraphNode& node, bool running, bool has_issue, int order);
    void draw_leaf_params(BtGraphNode& node);
    void handle_create();
    void handle_delete();
    void draw_context_menus();
    void read_back_positions();
    void draw_issues();
    void commit(scene::BehaviorTree& bt);
    void note(Change change);

    ax::NodeEditor::EditorContext* ctx_ = nullptr;

    entt::entity entity_ = entt::null;
    std::string  loaded_source_; // the source graph_ mirrors (or last wrote)
    std::string  load_error_;    // source that cannot be shown as a graph
    std::string  compile_error_; // valid graph the runtime still refused
    BtGraph      graph_;
    std::vector<BtGraphIssue> issues_;
    bool         unapplied_ = false; // graph_ differs from source (invalid)

    Change pending_ = Change::none;  // to commit after the canvas is drawn
    bool   dragging_layout_ = false; // node positions changed; commit on release
    bool   push_all_positions_ = true;
    std::vector<std::uint32_t> push_positions_; // nodes to place this frame
    int    navigate_in_frames_ = 0; // > 0: fit the view once nodes have sizes
    std::uint32_t focus_node_ = 0;  // issue clicked: select + centre it
    std::string   tooltip_;         // shown after the canvas (not inside nodes)

    // Context menus / node creation.
    std::uint32_t context_node_ = 0;
    std::uint32_t context_link_child_ = 0;
    std::uint64_t create_from_pin_ = 0; // pin a dropped link came from (0 = none)
    glm::vec2     create_pos_{0.0F};    // canvas position for a created node
};

} // namespace engine::editor
