// Behaviour-tree graph model (G2): JSON <-> graph round trips, layout
// persistence in the "editor" field, validation messages, and the edit
// operations the Behaviour panel and the remote bt.* methods use. CPU-only.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/behavior.hpp"
#include "engine/scene/bt_graph.hpp"
#include "support/expect_ok.hpp"

namespace {

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

[[nodiscard]] bool has_issue(const std::vector<s::BtGraphIssue>& issues, std::uint32_t node,
                             const std::string& fragment) {
    return std::ranges::any_of(issues, [&](const s::BtGraphIssue& i) {
        return i.node == node && i.message.find(fragment) != std::string::npos;
    });
}

[[nodiscard]] std::string dump(const std::vector<s::BtGraphIssue>& issues) {
    std::string out;
    for (const auto& i : issues) out += "[" + std::to_string(i.node) + "] " + i.message + "\n";
    return out;
}

// Structural + layout equality (ids, kinds, leaves, params, child order, pos).
void expect_same_graph(const s::BtGraph& a, const s::BtGraph& b) {
    ASSERT_EQ(a.nodes.size(), b.nodes.size());
    for (const s::BtGraphNode& na : a.nodes) {
        const s::BtGraphNode* nb = b.find(na.id);
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
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    ASSERT_EQ(g->nodes.size(), 8U);
    const std::uint32_t root = s::bt_graph_root(*g);
    ASSERT_NE(root, 0U);
    EXPECT_EQ(g->find(root)->kind, Kind::reactive_selector);
    EXPECT_EQ(g->find(root)->children.size(), 2U);
    EXPECT_TRUE(s::bt_graph_validate(*g).empty()) << dump(s::bt_graph_validate(*g));

    // move_to without y keeps y absent (absence means "snap to the ground").
    const auto move_to = std::ranges::find_if(g->nodes, [](const s::BtGraphNode& n) {
        return n.leaf == "move_to";
    });
    ASSERT_NE(move_to, g->nodes.end());
    ASSERT_EQ(move_to->params.size(), 3U);
    EXPECT_EQ(move_to->params[0].name, "x");
    EXPECT_EQ(move_to->params[1].name, "z");
    EXPECT_FLOAT_EQ(move_to->params[2].value, 2.5F);
}

TEST(BtGraph, RoundTripKeepsIdsLayoutAndCompiles) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    // Designer moves nodes around, then the tree is saved and reloaded.
    float x = 10.0F;
    for (s::BtGraphNode& n : g->nodes) {
        n.pos = {x, x * 0.5F + 3.0F};
        x += 37.0F;
    }
    auto text = s::bt_graph_to_json(*g);
    ASSERT_OK(text);
    auto again = s::bt_graph_from_json(*text);
    ASSERT_OK(again);
    expect_same_graph(*g, *again);
    // Stable: a second save is byte-identical.
    auto text2 = s::bt_graph_to_json(*again);
    ASSERT_OK(text2);
    EXPECT_EQ(*text, *text2);

    // The runtime compiles it, ignoring the editor field, with its nodes in
    // the graph's pre-order (the panel's running-node mapping relies on it).
    auto compiled = s::BehaviorTreeAsset::parse(*text);
    ASSERT_OK(compiled);
    const std::vector<std::uint32_t> order = s::bt_graph_preorder(*again);
    ASSERT_EQ((*compiled)->nodes().size(), order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ((*compiled)->nodes()[i].kind, again->find(order[i])->kind) << "node " << i;
    }
}

TEST(BtGraph, IdsSurviveEditsAcrossSaves) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    const std::uint32_t root = s::bt_graph_root(*g);
    const std::uint32_t wait = s::bt_graph_add_node(*g, Kind::leaf, "wait", {900.0F, 200.0F});
    ASSERT_OK(s::bt_graph_link(*g, root, wait));
    auto text = s::bt_graph_to_json(*g);
    ASSERT_OK(text);
    auto again = s::bt_graph_from_json(*text);
    ASSERT_OK(again);
    expect_same_graph(*g, *again);
    ASSERT_NE(again->find(wait), nullptr);
    EXPECT_GT(again->next_id, wait);
}

TEST(BtGraph, HandEditedShapeFallsBackToAutoLayout) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    auto text = s::bt_graph_to_json(*g);
    ASSERT_OK(text);
    // Remove a leaf by hand: the saved layout no longer matches the tree.
    nlohmann::json j = nlohmann::json::parse(*text);
    j["children"][1]["children"].erase(1);
    auto edited = s::bt_graph_from_json(j.dump());
    ASSERT_OK(edited);
    EXPECT_EQ(edited->nodes.size(), 7U);
    EXPECT_TRUE(s::bt_graph_validate(*edited).empty());
    // Auto layout: the root sits above its children.
    const s::BtGraphNode& r = *edited->find(s::bt_graph_root(*edited));
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
    EXPECT_ERR(s::bt_graph_from_json("{nope"));
    EXPECT_ERR(s::bt_graph_from_json(R"({"type":"parallel","children":[]})"));
    EXPECT_ERR(s::bt_graph_from_json(R"({"type":"leaf","name":"wait","seconds":"long"})"));
    EXPECT_ERR(s::bt_graph_from_json(R"({"type":"sequence","children":{},"x":1})"));
    EXPECT_ERR(s::bt_graph_from_json(
        R"({"type":"leaf","name":"wait","children":[{"type":"leaf","name":"wait"}]})"));
}

TEST(BtGraph, CycleIsReported) {
    s::BtGraph g;
    const std::uint32_t root = s::bt_graph_add_node(g, Kind::sequence, "", {});
    const std::uint32_t a = s::bt_graph_add_node(g, Kind::selector, "", {});
    const std::uint32_t b = s::bt_graph_add_node(g, Kind::sequence, "", {});
    g.find(root)->children = {a};
    g.find(a)->children = {b};
    g.find(b)->children = {a}; // a <-> b
    const auto issues = s::bt_graph_validate(g);
    EXPECT_TRUE(has_issue(issues, a, "part of a cycle")) << dump(issues);
    EXPECT_TRUE(has_issue(issues, a, "2 parents")) << dump(issues);
    EXPECT_ERR(s::bt_graph_to_json(g));
    // The edit API refuses to close a cycle in the first place.
    g.find(b)->children.clear();
    const auto refused = s::bt_graph_can_link(g, b, root);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("cycle"), std::string::npos);

    // A graph that is nothing but a cycle has no root at all.
    s::BtGraph ring;
    const std::uint32_t x = s::bt_graph_add_node(ring, Kind::sequence, "", {});
    const std::uint32_t y = s::bt_graph_add_node(ring, Kind::sequence, "", {});
    ring.find(x)->children = {y};
    ring.find(y)->children = {x};
    EXPECT_TRUE(has_issue(s::bt_graph_validate(ring), 0, "no root"));
    EXPECT_TRUE(s::bt_graph_preorder(ring).empty());
}

TEST(BtGraph, InverterTakesExactlyOneChild) {
    s::BtGraph g;
    const std::uint32_t inv = s::bt_graph_add_node(g, Kind::inverter, "", {});
    const std::uint32_t a = s::bt_graph_add_node(g, Kind::leaf, "wait", {});
    const std::uint32_t b = s::bt_graph_add_node(g, Kind::leaf, "wait", {});
    EXPECT_TRUE(has_issue(s::bt_graph_validate(g), inv, "at least one child"));
    ASSERT_OK(s::bt_graph_link(g, inv, a));
    EXPECT_ERR(s::bt_graph_link(g, inv, b)); // the editor refuses the second link
    g.find(inv)->children.push_back(b);      // ...a hand-built graph still gets it
    const auto issues = s::bt_graph_validate(g);
    EXPECT_TRUE(has_issue(issues, inv, "exactly one child (has 2)")) << dump(issues);
    auto text = s::bt_graph_to_json(g);
    ASSERT_FALSE(text.has_value());
    EXPECT_NE(text.error().message.find("exactly one child"), std::string::npos);
}

TEST(BtGraph, UnknownLeafAndBadParametersAreReported) {
    auto g = s::bt_graph_from_json(R"({"type":"sequence","children":[
        {"type":"leaf","name":"teleport","x":1},
        {"type":"leaf","name":"move_to","x":1,"z":2,"speed":-4},
        {"type":"leaf","name":"wait","secs":2}
    ]})");
    ASSERT_OK(g); // representable; semantics are validation's job
    const auto& kids = g->find(s::bt_graph_root(*g))->children;
    const auto issues = s::bt_graph_validate(*g);
    EXPECT_TRUE(has_issue(issues, kids[0], "unknown leaf 'teleport'")) << dump(issues);
    EXPECT_TRUE(has_issue(issues, kids[0], "known: move_to, wait, near_point, near_camera"));
    EXPECT_TRUE(has_issue(issues, kids[1], "move_to.speed: expected a number in [0, 1000]"))
        << dump(issues);
    EXPECT_TRUE(has_issue(issues, kids[2], "unknown parameter 'secs' (known: seconds)"))
        << dump(issues);
    EXPECT_EQ(issues.size(), 3U) << dump(issues);
    EXPECT_ERR(s::bt_graph_to_json(*g));
}

TEST(BtGraph, StrayNodesAndEmptyCompositesAreReported) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    const std::uint32_t stray = s::bt_graph_add_node(*g, Kind::selector, "", {});
    const auto issues = s::bt_graph_validate(*g);
    EXPECT_TRUE(has_issue(issues, stray, "not connected to the root 'reactive_selector'"))
        << dump(issues);
    EXPECT_TRUE(has_issue(issues, stray, "needs at least one child")) << dump(issues);
    EXPECT_TRUE(has_issue(s::bt_graph_validate(s::BtGraph{}), 0, "empty"));
}

TEST(BtGraph, LinkingMovesAChildAndRefusesLeafParents) {
    s::BtGraph g;
    const std::uint32_t root = s::bt_graph_add_node(g, Kind::selector, "", {});
    const std::uint32_t seq = s::bt_graph_add_node(g, Kind::sequence, "", {});
    const std::uint32_t leaf = s::bt_graph_add_node(g, Kind::leaf, "wait", {});
    EXPECT_ERR(s::bt_graph_link(g, leaf, seq));
    EXPECT_ERR(s::bt_graph_link(g, seq, seq));
    ASSERT_OK(s::bt_graph_link(g, root, seq));
    ASSERT_OK(s::bt_graph_link(g, root, leaf));
    ASSERT_OK(s::bt_graph_link(g, seq, leaf)); // re-parent
    EXPECT_EQ(g.find(root)->children, std::vector<std::uint32_t>{seq});
    EXPECT_EQ(s::bt_graph_parent(g, leaf), seq);
    EXPECT_TRUE(s::bt_graph_validate(g).empty()) << dump(s::bt_graph_validate(g));

    // Removing the middle node leaves its child parentless (a stray).
    s::bt_graph_remove_node(g, seq);
    EXPECT_EQ(g.find(root)->children.size(), 0U);
    EXPECT_EQ(s::bt_graph_parent(g, leaf), 0U);
}

TEST(BtGraph, ChildOrderFollowsCanvasX) {
    s::BtGraph g;
    const std::uint32_t root = s::bt_graph_add_node(g, Kind::sequence, "", {0.0F, 0.0F});
    const std::uint32_t a = s::bt_graph_add_node(g, Kind::leaf, "wait", {0.0F, 100.0F});
    const std::uint32_t b = s::bt_graph_add_node(g, Kind::leaf, "wait", {200.0F, 100.0F});
    const std::uint32_t c = s::bt_graph_add_node(g, Kind::leaf, "wait", {400.0F, 100.0F});
    for (const std::uint32_t k : {a, b, c}) ASSERT_OK(s::bt_graph_link(g, root, k));
    EXPECT_FALSE(s::bt_graph_sort_children_by_x(g));

    g.find(c)->pos.x = -50.0F; // dragged to the far left
    EXPECT_TRUE(s::bt_graph_sort_children_by_x(g));
    EXPECT_EQ(g.find(root)->children, (std::vector<std::uint32_t>{c, a, b}));

    // Explicit reorder swaps the screen positions too, so a later sort agrees.
    EXPECT_TRUE(s::bt_graph_move_child(g, b, -1));
    EXPECT_EQ(g.find(root)->children, (std::vector<std::uint32_t>{c, b, a}));
    EXPECT_FALSE(s::bt_graph_sort_children_by_x(g));
    EXPECT_FALSE(s::bt_graph_move_child(g, c, -1)); // already first
}

TEST(BtGraph, NewLeavesGetCatalogueDefaults) {
    s::BtGraph g;
    const std::uint32_t id = s::bt_graph_add_node(g, Kind::leaf, "move_to", {});
    const s::BtGraphNode& n = *g.find(id);
    // x, z, speed; y is optional (absent = snap to ground), so not added.
    ASSERT_EQ(n.params.size(), 3U);
    EXPECT_EQ(n.params[0].name, "x");
    EXPECT_EQ(n.params[1].name, "z");
    EXPECT_EQ(n.params[2].name, "speed");
    EXPECT_FLOAT_EQ(n.params[2].value, 3.0F);
    auto text = s::bt_graph_to_json(g);
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

// kGuard in pre-order: 1 reactive_selector, 2 sequence, 3 near_camera,
// 4 inverter, 5 near_point, 6 sequence, 7 move_to, 8 wait.

TEST(BtGraph, SubtreesAndPathsFollowTheJson) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    EXPECT_EQ(s::bt_graph_subtree(*g, 2), (std::vector<std::uint32_t>{2, 3, 4, 5}));
    EXPECT_TRUE(s::bt_graph_subtree(*g, 99).empty());
    EXPECT_EQ(s::bt_graph_path(*g, 1), "root");
    EXPECT_EQ(s::bt_graph_path(*g, 5), "root.children[0].children[1].children[0]");
    EXPECT_EQ(s::bt_graph_path(*g, 8), "root.children[1].children[1]");
    const std::uint32_t stray = s::bt_graph_add_node(*g, Kind::leaf, "wait", {});
    EXPECT_EQ(s::bt_graph_path(*g, stray), "") << "not under the root";
    EXPECT_EQ(s::bt_graph_path(*g, 99), "");
}

TEST(BtGraph, AttachPlacesAtAnIndexAndReorders) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    ASSERT_OK(s::bt_graph_attach(*g, 1, 8, 0)); // from the second sequence to the front
    EXPECT_EQ(g->find(1)->children, (std::vector<std::uint32_t>{8, 2, 6}));
    EXPECT_EQ(g->find(6)->children, (std::vector<std::uint32_t>{7}));
    ASSERT_OK(s::bt_graph_attach(*g, 1, 8, 2)); // re-order among the same siblings
    EXPECT_EQ(g->find(1)->children, (std::vector<std::uint32_t>{2, 6, 8}));
    ASSERT_OK(s::bt_graph_attach(*g, 6, 8)); // default: the end
    EXPECT_EQ(g->find(6)->children, (std::vector<std::uint32_t>{7, 8}));
    ASSERT_OK(s::bt_graph_attach(*g, 4, 5, 0)) << "an inverter re-ordering its only child";

    EXPECT_ERR(s::bt_graph_attach(*g, 6, 3, 3)) << "index past the end";
    EXPECT_ERR(s::bt_graph_attach(*g, 7, 3)) << "a leaf cannot parent";
    EXPECT_ERR(s::bt_graph_attach(*g, 4, 3)) << "the inverter already has its child";
    EXPECT_ERR(s::bt_graph_attach(*g, 2, 1)) << "cycle";
    EXPECT_ERR(s::bt_graph_attach(*g, 2, 99));
    EXPECT_EQ(g->find(2)->children, (std::vector<std::uint32_t>{3, 4})) << "refused: no change";
    EXPECT_TRUE(s::bt_graph_validate(*g).empty()) << dump(s::bt_graph_validate(*g));
}

TEST(BtGraph, PlacementKeepsScreenOrderWithChildOrder) {
    auto g = s::bt_graph_from_json(kGuard);
    ASSERT_OK(g);
    EXPECT_TRUE(s::bt_graph_has_auto_layout(*g));
    EXPECT_TRUE(s::bt_graph_children_follow_x(*g));

    // A new leaf between move_to and wait sits between them, one row down.
    const std::uint32_t extra = s::bt_graph_add_node(*g, Kind::leaf, "wait", {-500.0F, -500.0F});
    ASSERT_OK(s::bt_graph_attach(*g, 6, extra, 1));
    s::bt_graph_place_subtree(*g, extra);
    const glm::vec2 pos = g->find(extra)->pos;
    EXPECT_GT(pos.x, g->find(7)->pos.x);
    EXPECT_LT(pos.x, g->find(8)->pos.x);
    EXPECT_FLOAT_EQ(pos.y, g->find(7)->pos.y);
    EXPECT_TRUE(s::bt_graph_children_follow_x(*g));
    EXPECT_FALSE(s::bt_graph_has_auto_layout(*g));

    // A moved subtree keeps its shape.
    const glm::vec2 offset = g->find(5)->pos - g->find(2)->pos;
    ASSERT_OK(s::bt_graph_attach(*g, 6, 2));
    s::bt_graph_place_subtree(*g, 2);
    EXPECT_GT(g->find(2)->pos.y, g->find(6)->pos.y);
    EXPECT_NEAR(g->find(5)->pos.x - g->find(2)->pos.x, offset.x, 1e-3F);
    EXPECT_NEAR(g->find(5)->pos.y - g->find(2)->pos.y, offset.y, 1e-3F);
    EXPECT_TRUE(s::bt_graph_children_follow_x(*g));

    // Positions that contradict the child order are detected.
    std::swap(g->find(7)->pos, g->find(8)->pos);
    EXPECT_FALSE(s::bt_graph_children_follow_x(*g));
}

TEST(BtGraph, KindReflectionMatchesTheRuntime) {
    for (const s::BtKindInfo& k : s::bt_kind_info()) {
        EXPECT_EQ(s::BehaviorTreeAsset::kind_from_name(k.name), k.kind);
        EXPECT_EQ(s::BehaviorTreeAsset::kind_name(k.kind), k.name);
        EXPECT_FALSE(k.help.empty()) << k.name;
    }
    EXPECT_EQ(s::bt_find_kind_info(Kind::inverter).max_children, 1);
    auto bad = s::bt_graph_from_json(R"({"type":"sequnce","children":[]})");
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find("known: sequence"), std::string::npos)
        << bad.error().message;
}

} // namespace
