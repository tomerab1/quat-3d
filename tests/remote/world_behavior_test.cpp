// Behaviour-tree authoring (ROADMAP A7): bt.leaves, bt.get, bt.set, the edit
// methods and bt.status on a real Scene through RpcServer::call.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/behavior.hpp"
#include "engine/scene/bt_graph.hpp"
#include "engine/scene/components.hpp"
#include "remote/world_fixture.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;

// Pre-order ids: 1 reactive_selector, 2 sequence, 3 near_camera, 4 inverter,
// 5 near_point, 6 sequence, 7 move_to, 8 wait.
const Json kGuard = Json::parse(R"({"type": "reactive_selector", "children": [
  {"type": "sequence", "children": [
    {"type": "leaf", "name": "near_camera", "range": 6},
    {"type": "inverter", "children": [
      {"type": "leaf", "name": "near_point", "x": 1, "y": 0, "z": 2, "range": 1.5}]}]},
  {"type": "sequence", "children": [
    {"type": "leaf", "name": "move_to", "x": 4, "z": -3, "speed": 2.5},
    {"type": "leaf", "name": "wait", "seconds": 0.5}]}]})");

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class BehaviorCommands : public quat::test::WorldCommands {
protected:
    entt::entity npc = entt::null;
    Json         id;
    bool         playing = false;

    void SetUp() override {
        services.playing = [this] { return playing; };
        register_commands();
        npc = scene.create_entity("npc");
        id = Json(entt::to_integral(npc));
    }

    // `json` (an object) with this test's entity added.
    [[nodiscard]] Json with_entity(const char* json) const {
        Json p = Json::parse(json);
        p["entity"] = id;
        return p;
    }

    // The view after bt.set of kGuard.
    Json set_guard() { return ok("bt.set", {{"entity", id}, {"tree", kGuard}}); }

    [[nodiscard]] const std::string& source() { return reg().get<s::BehaviorTree>(npc).source; }

    // Node ids of a view, in its order.
    [[nodiscard]] static std::vector<std::uint32_t> ids(const Json& view) {
        std::vector<std::uint32_t> out;
        for (const Json& n : view["nodes"]) out.push_back(n["id"].get<std::uint32_t>());
        return out;
    }
    [[nodiscard]] static Json node(const Json& view, std::uint32_t node_id) {
        for (const Json& n : view["nodes"]) {
            if (n["id"] == node_id) return n;
        }
        ADD_FAILURE() << "no node " << node_id << " in " << view.dump();
        return Json::object();
    }
    [[nodiscard]] static std::vector<std::uint32_t> children(const Json& view, std::uint32_t n) {
        return node(view, n)["children"].get<std::vector<std::uint32_t>>();
    }
    // The stored graph, as the Behaviour panel would load it.
    [[nodiscard]] s::BtGraph stored_graph() {
        auto g = s::bt_graph_from_json(source());
        EXPECT_TRUE(g.has_value()) << source();
        return g ? *g : s::BtGraph{};
    }
};

TEST_F(BehaviorCommands, LeavesReflectTheRuntimeTables) {
    const Json r = ok("bt.leaves");
    ASSERT_EQ(r["kinds"].size(), s::bt_kind_info().size());
    for (const Json& k : r["kinds"]) {
        const auto kind = s::BehaviorTreeAsset::kind_from_name(k["name"].get<std::string>());
        ASSERT_TRUE(kind.has_value()) << k.dump();
        EXPECT_FALSE(k["help"].get<std::string>().empty());
        if (*kind == s::BehaviorTreeAsset::Kind::inverter) EXPECT_EQ(k["max_children"], 1);
        if (*kind == s::BehaviorTreeAsset::Kind::sequence) EXPECT_TRUE(k["max_children"].is_null());
        if (*kind == s::BehaviorTreeAsset::Kind::leaf) EXPECT_EQ(k["max_children"], 0);
    }
    ASSERT_EQ(r["leaves"].size(), s::bt_builtin_leaf_info().size());
    for (const Json& l : r["leaves"]) {
        const s::BtLeafInfo* info = s::bt_find_leaf_info(l["name"].get<std::string>());
        ASSERT_NE(info, nullptr) << l.dump();
        ASSERT_EQ(l["params"].size(), info->params.size());
    }
    Json move_to;
    for (const Json& l : r["leaves"]) {
        if (l["name"] == "move_to") move_to = l;
    }
    ASSERT_TRUE(move_to.is_object());
    const Json& y = move_to["params"][1];
    EXPECT_EQ(y["name"], "y");
    EXPECT_EQ(y["optional"], true);
    EXPECT_FALSE(y.contains("min")) << "unbounded ranges are omitted";
    const Json& speed = move_to["params"][3];
    EXPECT_EQ(speed["default"], 3.0);
    EXPECT_EQ(speed["min"], 0.0);
    EXPECT_EQ(speed["max"], 1000.0);
}

TEST_F(BehaviorCommands, SetStoresAValidTreeAndGetShowsTheGraph) {
    const Json view = ok("bt.set", {{"entity", "npc"}, {"tree", kGuard}, {"enabled", false}});
    ASSERT_TRUE(reg().all_of<s::BehaviorTree>(npc)) << "bt.set creates the component";
    const s::BehaviorTree& bt = reg().get<s::BehaviorTree>(npc);
    EXPECT_FALSE(bt.enabled);
    EXPECT_EQ(bt.compiled, nullptr) << "recompiled on the next tick";
    EXPECT_TRUE(s::BehaviorTreeAsset::parse(bt.source).has_value());
    EXPECT_TRUE(contains(bt.source, "\"editor\"")) << "the layout is stored like the panel's";

    EXPECT_EQ(view["entity"], id);
    EXPECT_EQ(view["root"], 1);
    EXPECT_EQ(ids(view), (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    EXPECT_TRUE(view["issues"].empty()) << view["issues"].dump();
    const Json mt = node(view, 7);
    EXPECT_EQ(mt["kind"], "leaf");
    EXPECT_EQ(mt["leaf"], "move_to");
    EXPECT_EQ(mt["params"], (Json{{"x", 4.0}, {"z", -3.0}, {"speed", 2.5}}));
    EXPECT_EQ(mt["path"], "root.children[1].children[0]");
    EXPECT_EQ(mt["position"].size(), 2U);
    EXPECT_EQ(node(view, 4)["kind"], "inverter");
    EXPECT_FALSE(node(view, 4).contains("params"));
    EXPECT_EQ(children(view, 1), (std::vector<std::uint32_t>{2, 6}));
    EXPECT_FALSE(view["tree"].contains("editor")) << "tree is the plain runtime JSON";
    EXPECT_EQ(view["tree"]["children"][1]["children"][1]["seconds"], 0.5);

    // bt.get shows the same; a tree given as JSON text is accepted too.
    EXPECT_EQ(ok("bt.get", {{"entity", id}}), view);
    ok("bt.set", {{"entity", id}, {"tree", R"({"type":"leaf","name":"wait"})"}});
    EXPECT_EQ(ok("bt.get", {{"entity", id}})["nodes"].size(), 1U);
}

TEST_F(BehaviorCommands, InvalidTreesAreRefusedWithPerNodeErrors) {
    set_guard();
    const std::string before = source();

    Json bad = kGuard;
    bad["children"][1]["children"][1]["seconds"] = -1;
    bad["children"][0]["children"][1]["children"].push_back(
        {{"type", "leaf"}, {"name", "wait"}});
    auto r = call("bt.set", {{"entity", id}, {"tree", bad}});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, code::invalid_params);
    std::string m = r.error().message;
    // The extra node shifts the pre-order ids: the wait is node 9 now.
    EXPECT_TRUE(contains(m, "node 9 at root.children[1].children[1]: wait.seconds: expected a "
                            "number in [0, 1e+06]"))
        << m;
    EXPECT_TRUE(contains(m, "node 4 at root.children[0].children[1]: inverter: takes exactly one "
                            "child"))
        << m;
    ASSERT_TRUE(r.error().data.contains("issues"));
    EXPECT_EQ(r.error().data["issues"].size(), 2U);
    EXPECT_EQ(r.error().data["issues"][0]["path"], "root.children[0].children[1]");
    EXPECT_EQ(source(), before) << "nothing was written";

    Json typo = kGuard;
    typo["children"][0]["type"] = "sequnce";
    m = err("bt.set", {{"entity", id}, {"tree", typo}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "tree: root.children[0]: unknown node type 'sequnce' (known: "
                            "sequence, selector"))
        << m;
    typo = kGuard;
    typo["children"][1]["children"][0]["name"] = "walk_to";
    m = err("bt.set", {{"entity", id}, {"tree", typo}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "node 7 at root.children[1].children[0]: unknown leaf 'walk_to' "
                            "(known: move_to, wait, near_point, near_camera)"))
        << m;
    typo = kGuard;
    typo["children"][1]["children"][0]["sped"] = 2;
    m = err("bt.set", {{"entity", id}, {"tree", typo}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "move_to: unknown parameter 'sped' (known: x, y, z, speed)")) << m;
    err("bt.set", {{"entity", id}, {"tree", "{not json"}}, code::invalid_params);
    err("bt.set", {{"entity", id}, {"tree", 3}}, code::invalid_params);
    err("bt.set", {{"entity", id}}, code::invalid_params);
    EXPECT_EQ(source(), before);

    // A refused tree does not create the component either.
    const entt::entity other = scene.create_entity("other");
    err("bt.set", {{"entity", "other"}, {"tree", typo}}, code::invalid_params);
    EXPECT_FALSE(reg().all_of<s::BehaviorTree>(other));
}

TEST_F(BehaviorCommands, MissingTreesAndUneditableSources) {
    std::string m = err("bt.get", {{"entity", id}}, code::not_found);
    EXPECT_TRUE(contains(m, "has no behaviour tree; create one with bt.set")) << m;
    err("bt.add_node", {{"entity", id}, {"parent", nullptr}, {"kind", "leaf"}, {"leaf", "wait"}},
        code::not_found);
    err("bt.status", {{"entity", id}}, code::not_found);
    err("bt.get", {{"entity", 999999}}, code::not_found);
    err("bt.get", Json::object(), code::invalid_params);
    err("bt.get", {{"entity", id}, {"verbose", true}}, code::invalid_params);

    // A source the graph cannot show (written by hand) is reported, not edited.
    ok("scene.update", {{"entity", id}, {"components", {{"behavior", {{"source", "nonsense"}}}}}});
    const Json view = ok("bt.get", {{"entity", id}});
    EXPECT_TRUE(view["nodes"].empty());
    EXPECT_TRUE(view["tree"].is_null());
    EXPECT_EQ(view["source"], "nonsense");
    ASSERT_EQ(view["issues"].size(), 1U);
    m = err("bt.set_params", {{"entity", id}, {"node", 1}, {"params", {{"seconds", 1}}}},
            code::failed);
    EXPECT_TRUE(contains(m, "replace it with bt.set")) << m;
    ok("bt.set", {{"entity", id}, {"tree", kGuard}}); // ...which works
}

TEST_F(BehaviorCommands, AddNodeKeepsIdsAndTheAutomaticLayout) {
    set_guard();
    Json view = ok("bt.add_node", {{"entity", id},
                                   {"parent", 6},
                                   {"index", 1},
                                   {"kind", "leaf"},
                                   {"leaf", "wait"},
                                   {"params", {{"seconds", 2}}}});
    EXPECT_EQ(view["node"], 9);
    EXPECT_EQ(children(view, 6), (std::vector<std::uint32_t>{7, 9, 8}));
    EXPECT_EQ(node(view, 9)["params"], (Json{{"seconds", 2.0}}));
    EXPECT_EQ(ids(view), (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 9, 8}))
        << "existing ids are kept; the view lists pre-order";

    // The stored layout carries the ids and is still the automatic one.
    const s::BtGraph g = stored_graph();
    EXPECT_EQ(s::bt_graph_preorder(g), ids(view));
    EXPECT_TRUE(s::bt_graph_has_auto_layout(g));
    EXPECT_EQ(g.next_id, 10U);

    // Ids survive further edits and re-reads.
    view = ok("bt.add_node", {{"entity", id}, {"parent", 2}, {"kind", "leaf"},
                              {"leaf", "near_camera"}});
    EXPECT_EQ(view["node"], 10);
    EXPECT_EQ(children(ok("bt.get", {{"entity", id}}), 2), (std::vector<std::uint32_t>{3, 4, 10}));
}

TEST_F(BehaviorCommands, EditsKeepAHandMadeLayout) {
    set_guard();
    // The designer moved things around in the panel.
    s::BtGraph g = stored_graph();
    for (s::BtGraphNode& n : g.nodes) n.pos += glm::vec2(37.0F * static_cast<float>(n.id), 11.0F);
    auto text = s::bt_graph_to_json(g);
    ASSERT_TRUE(text.has_value());
    reg().get<s::BehaviorTree>(npc).source = *text;
    g = stored_graph();
    ASSERT_FALSE(s::bt_graph_has_auto_layout(g));

    const Json view = ok("bt.add_node", {{"entity", id}, {"parent", 6}, {"index", 1},
                                         {"kind", "leaf"}, {"leaf", "wait"}});
    const s::BtGraph after = stored_graph();
    for (const s::BtGraphNode& n : g.nodes) {
        EXPECT_EQ(after.find(n.id)->pos, n.pos) << "node " << n.id << " stays where it was";
    }
    const glm::vec2 added = after.find(view["node"].get<std::uint32_t>())->pos;
    EXPECT_GT(added.x, after.find(7)->pos.x);
    EXPECT_LT(added.x, after.find(8)->pos.x);
    EXPECT_GT(added.y, after.find(6)->pos.y);
    EXPECT_TRUE(s::bt_graph_children_follow_x(after));
}

TEST_F(BehaviorCommands, AddCompositesWrapAndReRoot) {
    set_guard();
    // Wrap move_to (7) in an inverter, in its place.
    Json view = ok("bt.add_node", {{"entity", id}, {"parent", 6}, {"index", 0},
                                   {"kind", "inverter"}, {"children", {7}}});
    const std::uint32_t inv = view["node"];
    EXPECT_EQ(children(view, 6), (std::vector<std::uint32_t>{inv, 8}));
    EXPECT_EQ(children(view, inv), (std::vector<std::uint32_t>{7}));

    // A new subtree given as tree JSON gets fresh ids.
    view = ok("bt.add_node", with_entity(R"({"parent": 1, "kind": "sequence", "children": [
        {"type": "leaf", "name": "wait", "seconds": 1},
        {"type": "inverter", "children": [{"type": "leaf", "name": "near_camera"}]}]})"));
    const std::uint32_t seq = view["node"];
    EXPECT_EQ(seq, inv + 1);
    const std::vector<std::uint32_t> kids = children(view, seq);
    ASSERT_EQ(kids.size(), 2U);
    EXPECT_EQ(node(view, kids[0])["leaf"], "wait");
    EXPECT_EQ(node(view, kids[1])["kind"], "inverter");
    EXPECT_EQ(children(view, 1).back(), seq);

    // A new root must adopt the current one.
    std::string m = err("bt.add_node", {{"entity", id}, {"parent", nullptr}, {"kind", "selector"},
                                        {"children", {6}}},
                        code::invalid_params);
    EXPECT_TRUE(contains(m, "must adopt the current root (node 1)")) << m;
    view = ok("bt.add_node", with_entity(R"({"parent": null, "kind": "selector",
        "children": [1, {"type": "leaf", "name": "wait"}]})"));
    EXPECT_EQ(view["root"], view["node"]);
    EXPECT_EQ(node(view, 1)["path"], "root.children[0]");
    EXPECT_TRUE(s::bt_graph_has_auto_layout(stored_graph()));
}

TEST_F(BehaviorCommands, AddNodeErrorsNameTheField) {
    set_guard();
    const std::string before = source();
    const auto add = [&](Json extra) {
        Json p{{"entity", id}, {"parent", 6}};
        p.update(extra);
        return err("bt.add_node", p, code::invalid_params);
    };
    EXPECT_TRUE(contains(add({{"kind", "sequnce"}}), "kind: unknown kind 'sequnce' (known: "));
    EXPECT_TRUE(contains(add({{"kind", "leaf"}}), "leaf: required for kind \"leaf\" (one of: "));
    EXPECT_TRUE(contains(add({{"kind", "leaf"}, {"leaf", "fly"}}), "leaf: unknown leaf 'fly'"));
    EXPECT_TRUE(contains(add({{"kind", "sequence"}}), "children: a sequence needs at least one"));
    EXPECT_TRUE(contains(add({{"kind", "leaf"}, {"leaf", "wait"}, {"children", {7}}}),
                         "children: a leaf takes no children"));
    EXPECT_TRUE(contains(add({{"kind", "leaf"}, {"leaf", "wait"}, {"index", 5}}),
                         "index: expected 0..2 (node 6 has 2 other children)"));
    EXPECT_TRUE(contains(add({{"kind", "leaf"}, {"leaf", "wait"}, {"params", {{"secs", 1}}}}),
                         "wait: unknown parameter 'secs' (known: seconds)"));
    EXPECT_TRUE(contains(add({{"kind", "inverter"}, {"children", {7, 8}}}),
                         "children[1]: an inverter takes exactly one child"));
    EXPECT_TRUE(contains(add({{"kind", "sequence"}, {"children", {1}}}),
                         "parent: would create a cycle"));
    const Json leef = Json::parse(R"([{"type": "leef"}])");
    EXPECT_TRUE(contains(add({{"kind", "sequence"}, {"children", leef}}),
                         "children[0]: unknown node type 'leef'"));
    // Adding under a leaf is a validation-free structural refusal.
    std::string m = err("bt.add_node", {{"entity", id}, {"parent", 7}, {"kind", "leaf"},
                                        {"leaf", "wait"}},
                        code::invalid_params);
    EXPECT_TRUE(contains(m, "parent: a leaf cannot have children")) << m;
    m = err("bt.add_node", {{"entity", id}, {"parent", 42}, {"kind", "leaf"}, {"leaf", "wait"}},
            code::not_found);
    EXPECT_TRUE(contains(m, "parent: no node 42 in this tree (its nodes: 1, 2, 3")) << m;
    EXPECT_EQ(source(), before);
}

TEST_F(BehaviorCommands, RemoveAndMoveNodes) {
    set_guard();
    // Removing wait leaves move_to alone in its sequence.
    Json view = ok("bt.remove_node", {{"entity", id}, {"node", 8}});
    EXPECT_EQ(children(view, 6), (std::vector<std::uint32_t>{7}));
    EXPECT_EQ(view["nodes"].size(), 7U);
    // Removing the last child would leave an empty sequence: refused, named.
    std::string m = err("bt.remove_node", {{"entity", id}, {"node", 7}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "node 6 at root.children[1]: sequence: needs at least one child")) << m;

    // Splicing an inverter's child into its place.
    view = ok("bt.remove_node", {{"entity", id}, {"node", 4}, {"keep_children", true}});
    EXPECT_EQ(children(view, 2), (std::vector<std::uint32_t>{3, 5}));
    // Removing a whole subtree.
    view = ok("bt.remove_node", {{"entity", id}, {"node", 6}});
    EXPECT_EQ(ids(view), (std::vector<std::uint32_t>{1, 2, 3, 5}));

    m = err("bt.remove_node", {{"entity", id}, {"node", 1}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "is the root")) << m;
    // A root with one child can be spliced away: the child becomes the root.
    view = ok("bt.remove_node", {{"entity", id}, {"node", 1}, {"keep_children", true}});
    EXPECT_EQ(view["root"], 2);
    EXPECT_EQ(node(view, 2)["path"], "root");

    // Moving: re-order, re-parent, refusals.
    set_guard();
    view = ok("bt.move_node", {{"entity", id}, {"node", 8}, {"parent", 6}, {"index", 0}});
    EXPECT_EQ(children(view, 6), (std::vector<std::uint32_t>{8, 7}));
    view = ok("bt.move_node", {{"entity", id}, {"node", 3}, {"parent", 6}});
    EXPECT_EQ(children(view, 6), (std::vector<std::uint32_t>{8, 7, 3}));
    EXPECT_EQ(children(view, 2), (std::vector<std::uint32_t>{4}));
    EXPECT_EQ(node(view, 3)["path"], "root.children[1].children[2]");
    m = err("bt.move_node", {{"entity", id}, {"node", 2}, {"parent", 5}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "parent: a leaf cannot have children")) << m;
    m = err("bt.move_node", {{"entity", id}, {"node", 6}, {"parent", 6}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "parent: a node cannot be its own child")) << m;
    m = err("bt.move_node", {{"entity", id}, {"node", 2}, {"parent", 4}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "parent: an inverter takes exactly one child")) << m;
    m = err("bt.move_node", {{"entity", id}, {"node", 1}, {"parent", 2}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "is the root and cannot be moved")) << m;
    m = err("bt.move_node", {{"entity", id}, {"node", 7}, {"parent", 6}, {"index", 3}},
            code::invalid_params);
    EXPECT_TRUE(contains(m, "index: expected 0..2")) << m;
    // Moving 2's last child away would empty it.
    m = err("bt.move_node", {{"entity", id}, {"node", 4}, {"parent", 6}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "node 2 at root.children[0]: sequence: needs at least one child"))
        << m;
}

TEST_F(BehaviorCommands, SetParamsMergesAndValidates) {
    set_guard();
    Json view = ok("bt.set_params",
                   {{"entity", id}, {"node", 7}, {"params", {{"speed", 4}, {"y", 1.5}}}});
    EXPECT_EQ(node(view, 7)["params"], (Json{{"x", 4.0}, {"y", 1.5}, {"z", -3.0}, {"speed", 4.0}}))
        << "merged, in the leaf's declared order";
    view = ok("bt.set_params", {{"entity", id}, {"node", 7}, {"params", {{"y", nullptr}}}});
    EXPECT_FALSE(node(view, 7)["params"].contains("y")) << "null removes (default / ground snap)";
    EXPECT_EQ(view["tree"]["children"][1]["children"][0]["speed"], 4.0);

    std::string m = err("bt.set_params", {{"entity", id}, {"node", 7}, {"params", {{"speed", -1}}}},
                        code::invalid_params);
    EXPECT_TRUE(contains(m, "node 7 at root.children[1].children[0]: move_to.speed: expected a "
                            "number in [0, 1000]"))
        << m;
    m = err("bt.set_params", {{"entity", id}, {"node", 7}, {"params", {{"speed", "fast"}}}},
            code::invalid_params);
    EXPECT_TRUE(contains(m, "params.speed: expected a number (or null to remove it)")) << m;
    m = err("bt.set_params", {{"entity", id}, {"node", 6}, {"params", {{"speed", 1}}}},
            code::invalid_params);
    EXPECT_TRUE(contains(m, "node: 6 is a sequence; only leaves have parameters")) << m;
    err("bt.set_params", {{"entity", id}, {"node", 7}}, code::invalid_params);
    err("bt.set_params", {{"entity", id}, {"node", "seven"}, {"params", {}}},
        code::invalid_params);
}

TEST_F(BehaviorCommands, SetWithTheSameShapeKeepsIdsAndLayout) {
    set_guard();
    ok("bt.move_node", {{"entity", id}, {"node", 8}, {"parent", 6}, {"index", 0}});
    Json view = ok("bt.get", {{"entity", id}});
    const std::vector<std::uint32_t> before = ids(view);
    const s::BtGraph layout = stored_graph();

    // The model edits the plain tree from bt.get and sends it back.
    Json tree = view["tree"];
    tree["children"][1]["children"][0]["seconds"] = 3;
    view = ok("bt.set", {{"entity", id}, {"tree", tree}});
    EXPECT_EQ(ids(view), before);
    EXPECT_EQ(node(view, 8)["params"], (Json{{"seconds", 3.0}}));
    const s::BtGraph after = stored_graph();
    for (const s::BtGraphNode& n : layout.nodes) EXPECT_EQ(after.find(n.id)->pos, n.pos);

    // A different shape starts over with pre-order ids.
    tree["children"].erase(0);
    view = ok("bt.set", {{"entity", id}, {"tree", tree}});
    EXPECT_EQ(ids(view), (std::vector<std::uint32_t>{1, 2, 3, 4}));
}

TEST_F(BehaviorCommands, StatusReportsTheLastTick) {
    bool playing = false;
    services.playing = [&] { return playing; };
    server = engine::remote::RpcServer{};
    register_commands();

    // 1 selector, 2 near_camera (fails: no camera), 3 wait (running).
    ok("bt.set", with_entity(R"({"tree": {"type": "selector", "children": [
        {"type": "leaf", "name": "near_camera"},
        {"type": "leaf", "name": "wait", "seconds": 5}]}})"));
    Json st = ok("bt.status", {{"entity", id}});
    EXPECT_EQ(st["compiled"], false);
    EXPECT_EQ(st["playing"], false);
    EXPECT_TRUE(st["nodes"].empty());
    EXPECT_TRUE(st["root_status"].is_null());

    playing = true;
    s::bt_system(reg(), 0.1F);
    st = ok("bt.status", {{"entity", id}});
    EXPECT_EQ(st["playing"], true);
    EXPECT_EQ(st["compiled"], true);
    EXPECT_EQ(st["tick"], 1);
    EXPECT_EQ(st["root_status"], "running");
    EXPECT_EQ(st["nodes"], Json::parse(R"([
        {"id": 1, "path": "root", "status": "running"},
        {"id": 2, "path": "root.children[0]", "status": "failure"},
        {"id": 3, "path": "root.children[1]", "status": "running"}])"));

    // The selector resumes at the running wait: near_camera is not reached.
    s::bt_system(reg(), 0.1F);
    st = ok("bt.status", {{"entity", id}});
    EXPECT_EQ(st["nodes"][1]["status"], "idle");

    // An edit recompiles: no status until the next tick.
    ok("bt.set_params", {{"entity", id}, {"node", 3}, {"params", {{"seconds", 1}}}});
    st = ok("bt.status", {{"entity", id}});
    EXPECT_EQ(st["compiled"], false);
    EXPECT_TRUE(st["nodes"].empty());
}

} // namespace
