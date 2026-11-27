// Behaviour-tree graph model (G2): JSON <-> graph round trips, layout
// persistence in the "editor" field, validation messages, and the edit
// operations the Behaviour panel uses. CPU-only, no ImGui.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/bt_graph.hpp"
#include "engine/scene/behavior.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace ed = engine::editor;
namespace s = engine::scene;
using Kind = s::BehaviorTreeAsset::Kind;

constexpr const char* kGuard = R"({
  "type": "reactive_selector",
  "children": [
    {"type": "sequence", "children": [
      {"type": "leaf", "name": "near_camera", "range": 6},
      {"type": "inverter", "children": [
        {"type": "leaf", "name": "near_point", "x": 1, "y": 0, "z": 2, "range": 1.5}
      ]}
    ]},
    {"type": "sequence", "children": [
      {"type": "leaf", "name": "move_to", "x": 4, "z": -3, "speed": 2.5},
      {"type": "leaf", "name": "wait", "seconds": 0.1}
    ]}
  ]
})";

[[nodiscard]] bool has_issue(const std::vector<ed::BtGraphIssue>& issues, std::uint32_t node,
                             const std::string& fragment) {
    return std::ranges::any_of(issues, [&](const ed::BtGraphIssue& i) {
        return i.node == node && i.message.find(fragment) != std::string::npos;
    });
}

[[nodiscard]] std::string dump(const std::vector<ed::BtGraphIssue>& issues) {
    std::string out;
    for (const auto& i : issues) out += "[" + std::to_string(i.node) + "] " + i.message + "\n";
    return out;
}

// Structural + layout equality (ids, kinds, leaves, params, child order, pos).
void expect_same_graph(const ed::BtGraph& a, const ed::BtGraph& b) {
    ASSERT_EQ(a.nodes.size(), b.nodes.size());
    for (const ed::BtGraphNode& na : a.nodes) {
        const ed::BtGraphNode* nb = b.find(na.id);
        ASSERT_NE(nb, nullptr) << "node " << na.id;
        EXPECT_EQ(na.kind, nb->kind);
        EXPECT_EQ(na.leaf, nb->leaf);
        EXPECT_EQ(na.children, nb->children);
        EXPECT_NEAR(na.pos.x, nb->pos.x, 0.5F);
        EXPECT_NEAR(na.pos.y, nb->pos.y, 0.5F);
        ASSERT_EQ(na.params.size(), nb->params.size());
        for (std::size_t i = 0; i < na.params.size(); ++i) {
            EXPECT_EQ(na.params[i].name, nb->params[i].name);
            EXPECT_FLOAT_EQ(na.params[i].value, nb->params[i].value);
        }
    }
}

TEST(BtGraph, ReadsTreeStructureAndParameters) {
    auto g = ed::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    ASSERT_EQ(g->nodes.size(), 8U);
    const std::uint32_t root = ed::bt_graph_root(*g);
    ASSERT_NE(root, 0U);
    EXPECT_EQ(g->find(root)->kind, Kind::reactive_selector);
    EXPECT_EQ(g->find(root)->children.size(), 2U);
    EXPECT_TRUE(ed::bt_graph_validate(*g).empty()) << dump(ed::bt_graph_validate(*g));

    // move_to without y keeps y absent (absence means "snap to the ground").
    const auto move_to = std::ranges::find_if(g->nodes, [](const ed::BtGraphNode& n) {
        return n.leaf == "move_to";
    });
    ASSERT_NE(move_to, g->nodes.end());
    ASSERT_EQ(move_to->params.size(), 3U);
    EXPECT_EQ(move_to->params[0].name, "x");
    EXPECT_EQ(move_to->params[1].name, "z");
    EXPECT_FLOAT_EQ(move_to->params[2].value, 2.5F);
}

TEST(BtGraph, RoundTripKeepsIdsLayoutAndCompiles) {
    auto g = ed::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    // Designer moves nodes around, then the tree is saved and reloaded.
    float x = 10.0F;
    for (ed::BtGraphNode& n : g->nodes) {
        n.pos = {x, x * 0.5F + 3.0F};
        x += 37.0F;
    }
    auto text = ed::bt_graph_to_json(*g);
    ASSERT_OK(text);
    auto again = ed::bt_graph_from_json(*text);
    ASSERT_OK(again);
    expect_same_graph(*g, *again);
    // Stable: a second save is byte-identical.
    auto text2 = ed::bt_graph_to_json(*again);
    ASSERT_OK(text2);
    EXPECT_EQ(*text, *text2);

    // The runtime compiles it, ignoring the editor field, with its nodes in
    // the graph's pre-order (the panel's running-node mapping relies on it).
    auto compiled = s::BehaviorTreeAsset::parse(*text);
    ASSERT_OK(compiled);
    const std::vector<std::uint32_t> order = ed::bt_graph_preorder(*again);
    ASSERT_EQ((*compiled)->nodes().size(), order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ((*compiled)->nodes()[i].kind, again->find(order[i])->kind) << "node " << i;
    }
}

TEST(BtGraph, IdsSurviveEditsAcrossSaves) {
    auto g = ed::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    const std::uint32_t root = ed::bt_graph_root(*g);
    const std::uint32_t wait = ed::bt_graph_add_node(*g, Kind::leaf, "wait", {900.0F, 200.0F});
    ASSERT_OK(ed::bt_graph_link(*g, root, wait));
    auto text = ed::bt_graph_to_json(*g);
    ASSERT_OK(text);
    auto again = ed::bt_graph_from_json(*text);
    ASSERT_OK(again);
    expect_same_graph(*g, *again);
    ASSERT_NE(again->find(wait), nullptr);
    EXPECT_GT(again->next_id, wait);
}

TEST(BtGraph, HandEditedShapeFallsBackToAutoLayout) {
    auto g = ed::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    auto text = ed::bt_graph_to_json(*g);
    ASSERT_OK(text);
    // Remove a leaf by hand: the saved layout no longer matches the tree.
    nlohmann::json j = nlohmann::json::parse(*text);
    j["children"][1]["children"].erase(1);
    auto edited = ed::bt_graph_from_json(j.dump());
    ASSERT_OK(edited);
    EXPECT_EQ(edited->nodes.size(), 7U);
    EXPECT_TRUE(ed::bt_graph_validate(*edited).empty());
    // Auto layout: the root sits above its children.
    const ed::BtGraphNode& r = *edited->find(ed::bt_graph_root(*edited));
    for (const std::uint32_t c : r.children) EXPECT_GT(edited->find(c)->pos.y, r.pos.y);
}

TEST(BtGraph, RuntimeIgnoresEditorFieldOnlyAtTheRoot) {
    EXPECT_OK(s::BehaviorTreeAsset::parse(
        R"({"type":"leaf","name":"wait","seconds":1,"editor":{"anything":[1,2]}})"));
    EXPECT_OK(s::BehaviorTreeAsset::parse(
        R"({"type":"sequence","editor":7,"children":[{"type":"leaf","name":"wait"}]})"));
    // Below the root it is still a typo-checked unknown field.
    EXPECT_ERR(s::BehaviorTreeAsset::parse(
        R"({"type":"sequence","children":[{"type":"leaf","name":"wait","editor":{}}]})"));
}

TEST(BtGraph, RejectsJsonItCannotRepresent) {
    EXPECT_ERR(ed::bt_graph_from_json("{nope"));
    EXPECT_ERR(ed::bt_graph_from_json(R"({"type":"parallel","children":[]})"));
    EXPECT_ERR(ed::bt_graph_from_json(R"({"type":"leaf","name":"wait","seconds":"long"})"));
    EXPECT_ERR(ed::bt_graph_from_json(R"({"type":"sequence","children":{},"x":1})"));
    EXPECT_ERR(ed::bt_graph_from_json(
        R"({"type":"leaf","name":"wait","children":[{"type":"leaf","name":"wait"}]})"));
}

TEST(BtGraph, CycleIsReported) {
    ed::BtGraph g;
    const std::uint32_t root = ed::bt_graph_add_node(g, Kind::sequence, "", {});
    const std::uint32_t a = ed::bt_graph_add_node(g, Kind::selector, "", {});
    const std::uint32_t b = ed::bt_graph_add_node(g, Kind::sequence, "", {});
    g.find(root)->children = {a};
    g.find(a)->children = {b};
    g.find(b)->children = {a}; // a <-> b
    const auto issues = ed::bt_graph_validate(g);
    EXPECT_TRUE(has_issue(issues, a, "part of a cycle")) << dump(issues);
    EXPECT_TRUE(has_issue(issues, a, "2 parents")) << dump(issues);
    EXPECT_ERR(ed::bt_graph_to_json(g));
    // The edit API refuses to close a cycle in the first place.
    g.find(b)->children.clear();
    const auto refused = ed::bt_graph_can_link(g, b, root);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("cycle"), std::string::npos);

    // A graph that is nothing but a cycle has no root at all.
    ed::BtGraph ring;
    const std::uint32_t x = ed::bt_graph_add_node(ring, Kind::sequence, "", {});
    const std::uint32_t y = ed::bt_graph_add_node(ring, Kind::sequence, "", {});
    ring.find(x)->children = {y};
    ring.find(y)->children = {x};
    EXPECT_TRUE(has_issue(ed::bt_graph_validate(ring), 0, "no root"));
    EXPECT_TRUE(ed::bt_graph_preorder(ring).empty());
}

TEST(BtGraph, InverterTakesExactlyOneChild) {
    ed::BtGraph g;
    const std::uint32_t inv = ed::bt_graph_add_node(g, Kind::inverter, "", {});
    const std::uint32_t a = ed::bt_graph_add_node(g, Kind::leaf, "wait", {});
    const std::uint32_t b = ed::bt_graph_add_node(g, Kind::leaf, "wait", {});
    EXPECT_TRUE(has_issue(ed::bt_graph_validate(g), inv, "at least one child"));
    ASSERT_OK(ed::bt_graph_link(g, inv, a));
    EXPECT_ERR(ed::bt_graph_link(g, inv, b)); // the editor refuses the second link
    g.find(inv)->children.push_back(b);      // ...a hand-built graph still gets it
    const auto issues = ed::bt_graph_validate(g);
    EXPECT_TRUE(has_issue(issues, inv, "exactly one child (has 2)")) << dump(issues);
    auto text = ed::bt_graph_to_json(g);
    ASSERT_FALSE(text.has_value());
    EXPECT_NE(text.error().message.find("exactly one child"), std::string::npos);
}

TEST(BtGraph, UnknownLeafAndBadParametersAreReported) {
    auto g = ed::bt_graph_from_json(R"({"type":"sequence","children":[
        {"type":"leaf","name":"teleport","x":1},
        {"type":"leaf","name":"move_to","x":1,"z":2,"speed":-4},
        {"type":"leaf","name":"wait","secs":2}
    ]})");
    ASSERT_OK(g); // representable; semantics are validation's job
    const auto& kids = g->find(ed::bt_graph_root(*g))->children;
    const auto issues = ed::bt_graph_validate(*g);
    EXPECT_TRUE(has_issue(issues, kids[0], "unknown leaf 'teleport'")) << dump(issues);
    EXPECT_TRUE(has_issue(issues, kids[0], "known: move_to, wait, near_point, near_camera"));
    EXPECT_TRUE(has_issue(issues, kids[1], "move_to.speed: expected a number in [0, 1000]"))
        << dump(issues);
    EXPECT_TRUE(has_issue(issues, kids[2], "unknown parameter 'secs' (known: seconds)"))
        << dump(issues);
    EXPECT_EQ(issues.size(), 3U) << dump(issues);
    EXPECT_ERR(ed::bt_graph_to_json(*g));
}

TEST(BtGraph, StrayNodesAndEmptyCompositesAreReported) {
    auto g = ed::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    const std::uint32_t stray = ed::bt_graph_add_node(*g, Kind::selector, "", {});
    const auto issues = ed::bt_graph_validate(*g);
    EXPECT_TRUE(has_issue(issues, stray, "not connected to the root 'reactive_selector'"))
        << dump(issues);
    EXPECT_TRUE(has_issue(issues, stray, "needs at least one child")) << dump(issues);
    EXPECT_TRUE(has_issue(ed::bt_graph_validate(ed::BtGraph{}), 0, "empty"));
}

TEST(BtGraph, LinkingMovesAChildAndRefusesLeafParents) {
    ed::BtGraph g;
    const std::uint32_t root = ed::bt_graph_add_node(g, Kind::selector, "", {});
    const std::uint32_t seq = ed::bt_graph_add_node(g, Kind::sequence, "", {});
    const std::uint32_t leaf = ed::bt_graph_add_node(g, Kind::leaf, "wait", {});
    EXPECT_ERR(ed::bt_graph_link(g, leaf, seq));
    EXPECT_ERR(ed::bt_graph_link(g, seq, seq));
    ASSERT_OK(ed::bt_graph_link(g, root, seq));
    ASSERT_OK(ed::bt_graph_link(g, root, leaf));
    ASSERT_OK(ed::bt_graph_link(g, seq, leaf)); // re-parent
    EXPECT_EQ(g.find(root)->children, std::vector<std::uint32_t>{seq});
    EXPECT_EQ(ed::bt_graph_parent(g, leaf), seq);
    EXPECT_TRUE(ed::bt_graph_validate(g).empty()) << dump(ed::bt_graph_validate(g));

    // Removing the middle node leaves its child parentless (a stray).
    ed::bt_graph_remove_node(g, seq);
    EXPECT_EQ(g.find(root)->children.size(), 0U);
    EXPECT_EQ(ed::bt_graph_parent(g, leaf), 0U);
}

TEST(BtGraph, ChildOrderFollowsCanvasX) {
    ed::BtGraph g;
    const std::uint32_t root = ed::bt_graph_add_node(g, Kind::sequence, "", {0.0F, 0.0F});
    const std::uint32_t a = ed::bt_graph_add_node(g, Kind::leaf, "wait", {0.0F, 100.0F});
    const std::uint32_t b = ed::bt_graph_add_node(g, Kind::leaf, "wait", {200.0F, 100.0F});
    const std::uint32_t c = ed::bt_graph_add_node(g, Kind::leaf, "wait", {400.0F, 100.0F});
    for (const std::uint32_t k : {a, b, c}) ASSERT_OK(ed::bt_graph_link(g, root, k));
    EXPECT_FALSE(ed::bt_graph_sort_children_by_x(g));

    g.find(c)->pos.x = -50.0F; // dragged to the far left
    EXPECT_TRUE(ed::bt_graph_sort_children_by_x(g));
    EXPECT_EQ(g.find(root)->children, (std::vector<std::uint32_t>{c, a, b}));

    // Explicit reorder swaps the screen positions too, so a later sort agrees.
    EXPECT_TRUE(ed::bt_graph_move_child(g, b, -1));
    EXPECT_EQ(g.find(root)->children, (std::vector<std::uint32_t>{c, b, a}));
    EXPECT_FALSE(ed::bt_graph_sort_children_by_x(g));
    EXPECT_FALSE(ed::bt_graph_move_child(g, c, -1)); // already first
}

TEST(BtGraph, NewLeavesGetCatalogueDefaults) {
    ed::BtGraph g;
    const std::uint32_t id = ed::bt_graph_add_node(g, Kind::leaf, "move_to", {});
    const ed::BtGraphNode& n = *g.find(id);
    // x, z, speed; y is optional (absent = snap to ground), so not added.
    ASSERT_EQ(n.params.size(), 3U);
    EXPECT_EQ(n.params[0].name, "x");
    EXPECT_EQ(n.params[1].name, "z");
    EXPECT_EQ(n.params[2].name, "speed");
    EXPECT_FLOAT_EQ(n.params[2].value, 3.0F);
    auto text = ed::bt_graph_to_json(g);
    ASSERT_OK(text);
    EXPECT_OK(s::BehaviorTreeAsset::parse(*text));
}

// The reflection table is what the runtime factories enforce: defaults parse,
// and every declared bound is the runtime's bound.
TEST(BtGraph, LeafReflectionMatchesRuntime) {
    for (const s::BtLeafInfo& leaf : s::bt_builtin_leaf_info()) {
        nlohmann::json node = {{"type", "leaf"}, {"name", std::string(leaf.name)}};
        EXPECT_OK(s::BehaviorTreeAsset::parse(node.dump())) << leaf.name;
        for (const s::BtParamInfo& p : leaf.params) {
            nlohmann::json ok = node;
            ok[std::string(p.name)] = p.default_value;
            EXPECT_OK(s::BehaviorTreeAsset::parse(ok.dump())) << leaf.name << "." << p.name;
            if (p.min > std::numeric_limits<float>::lowest()) {
                nlohmann::json low = node;
                low[std::string(p.name)] = static_cast<double>(p.min) - 1.0;
                EXPECT_ERR(s::BehaviorTreeAsset::parse(low.dump())) << leaf.name << "." << p.name;
            }
            if (p.max < std::numeric_limits<float>::max()) {
                nlohmann::json high = node;
                high[std::string(p.name)] = static_cast<double>(p.max) * 2.0 + 1.0;
                EXPECT_ERR(s::BehaviorTreeAsset::parse(high.dump())) << leaf.name << "." << p.name;
            }
        }
        nlohmann::json typo = node;
        typo["not_a_param"] = 1;
        EXPECT_ERR(s::BehaviorTreeAsset::parse(typo.dump())) << leaf.name;
    }
}

} // namespace

#endif // ENGINE_EDITOR
