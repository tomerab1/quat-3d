// bt.add_node, bt.remove_node, bt.move_node, bt.set_params — behaviour-tree
// edits by node id (ROADMAP A7). Each edit runs on a copy of the entity's
// graph, keeps the editor layout consistent, and is stored only when the
// resulting tree validates (behavior_commands.cpp).

#include <algorithm>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "behavior_internal.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::remote::world {

namespace {

using scene::BtGraph;
using scene::BtGraphNode;
using scene::BtKind;

constexpr int k_max_index = 1 << 20;

// An edit in progress: the entity's tree and a working copy of its graph.
struct Edit {
    entt::entity         entity = entt::null;
    scene::BehaviorTree* tree = nullptr;
    BtGraph              graph;
    bool                 tidy = false; // the layout was the automatic one
};

// Resolves `entity` and loads its graph. Reads the field from `f`.
std::expected<Edit, RpcError> open_edit(Context& ctx, core::JsonFields& f) {
    entt::registry& r = ctx.scene->registry();
    auto e = required_entity(f, r, "entity");
    if (!e) return std::unexpected(e.error());
    auto tree = bt::tree_of(r, *e);
    if (!tree) return std::unexpected(tree.error());
    auto graph = bt::load_graph(**tree);
    if (!graph) return std::unexpected(graph.error());
    Edit edit{*e, *tree, std::move(*graph), false};
    edit.tidy = !edit.graph.nodes.empty() && scene::bt_graph_has_auto_layout(edit.graph);
    return edit;
}

// Lays out what the edit placed, validates and stores the graph, and returns
// the tree view. An automatically laid-out tree stays automatic; otherwise
// `placed` nodes go between their siblings, unless that would contradict
// child order on screen.
RpcResult commit_edit(Context& ctx, Edit& edit, std::span<const std::uint32_t> placed,
                      bool relayout) {
    if (edit.tidy || relayout) {
        scene::bt_graph_auto_layout(edit.graph);
    } else {
        for (const std::uint32_t id : placed) scene::bt_graph_place_subtree(edit.graph, id);
        if (!scene::bt_graph_children_follow_x(edit.graph)) scene::bt_graph_auto_layout(edit.graph);
    }
    auto text = bt::compile(edit.graph);
    if (!text) return std::unexpected(text.error());
    bt::write_source(*edit.tree, std::move(*text));
    return bt::tree_view(ctx.scene->registry(), edit.entity);
}

// Merges `params` ({name: number | null}) into a leaf's parameters; null
// removes one (back to its default) when `allow_null`.
std::expected<void, RpcError> merge_params(BtGraphNode& node, const Json& params,
                                           bool allow_null) {
    if (!params.is_object()) {
        return bad_params("params: expected an object of parameter values, e.g. {\"seconds\": 2}");
    }
    for (const auto& [name, value] : params.items()) {
        if (value.is_null() && allow_null) {
            std::erase_if(node.params,
                          [&](const scene::BtGraphParam& p) { return p.name == name; });
            continue;
        }
        float v = 0.0F;
        if (!core::JsonFields::read_float(value, v)) {
            return bad_params("params." + name + ": expected a number" +
                              (allow_null ? " (or null to remove it)" : ""));
        }
        const auto it = std::ranges::find(node.params, name, &scene::BtGraphParam::name);
        if (it != node.params.end()) {
            it->value = v;
        } else {
            node.params.push_back({name, v});
        }
    }
    return {};
}

// Adds the tree-JSON subtree `node` to `graph` with fresh ids; returns its
// root's id.
std::expected<std::uint32_t, RpcError> graft(BtGraph& graph, const Json& node,
                                             const std::string& field) {
    if (!node.is_object()) {
        return bad_params(field + ": expected a tree node object or the id of an existing node");
    }
    if (node.contains(std::string(scene::bt_editor_field))) {
        return bad_params(field + ": \"" + std::string(scene::bt_editor_field) +
                          "\" is only allowed at the root of a whole tree (bt.set)");
    }
    auto sub = scene::bt_graph_from_json(node.dump(-1, ' ', false, Json::error_handler_t::replace));
    if (!sub) return bad_params(bt::rebase_error(sub.error().message, field));
    std::unordered_map<std::uint32_t, std::uint32_t> ids;
    for (const BtGraphNode& n : sub->nodes) ids[n.id] = graph.next_id++;
    for (BtGraphNode n : sub->nodes) {
        n.id = ids[n.id];
        for (std::uint32_t& c : n.children) c = ids[c];
        graph.nodes.push_back(std::move(n));
    }
    return ids[scene::bt_graph_root(*sub)];
}

// Checks `index` against `parent`'s children other than `node`.
std::expected<void, RpcError> check_index(const BtGraph& graph, std::uint32_t parent,
                                          std::uint32_t node, int index) {
    const auto& children = graph.find(parent)->children;
    const auto others = static_cast<int>(children.size()) -
                        (std::ranges::find(children, node) != children.end() ? 1 : 0);
    if (index > others) {
        return bad_params("index: expected 0.." + std::to_string(others) + " (node " +
                          std::to_string(parent) + " has " + std::to_string(others) +
                          " other children)");
    }
    return {};
}

RpcResult bt_add_node(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto edit = open_edit(ctx, f);
    const Json* parent_ref = f.raw("parent");
    int index = 0;
    const bool has_index = f.has("index");
    f.integer("index", index, 0, k_max_index);
    std::string kind_name;
    std::string leaf;
    const bool has_leaf = f.has("leaf");
    f.string("kind", kind_name, 64).string("leaf", leaf, 64);
    const Json* leaf_params = f.raw("params");
    const Json* children = f.raw("children");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!edit) return std::unexpected(edit.error());
    BtGraph& g = edit->graph;

    if (kind_name.empty()) return bad_params("kind: required (one of: " + bt::known_kinds() + ")");
    const auto kind = scene::BehaviorTreeAsset::kind_from_name(kind_name);
    if (!kind) {
        return bad_params("kind: unknown kind '" + kind_name + "' (known: " + bt::known_kinds() +
                          ")");
    }
    const bool has_children = children != nullptr && !children->is_null();
    const bool has_params = leaf_params != nullptr && !leaf_params->is_null();
    if (*kind == BtKind::leaf) {
        if (leaf.empty()) {
            return bad_params("leaf: required for kind \"leaf\" (one of: " + bt::known_leaves() +
                              ")");
        }
        if (scene::bt_find_leaf_info(leaf) == nullptr) {
            return bad_params("leaf: unknown leaf '" + leaf + "' (known: " + bt::known_leaves() +
                              ")");
        }
        if (has_children) return bad_params("children: a leaf takes no children");
    } else {
        if (has_leaf) return bad_params("leaf: only for kind \"leaf\"");
        if (has_params) return bad_params("params: only leaves take parameters");
        if (!has_children || !children->is_array() || children->empty()) {
            return bad_params("children: a " + kind_name +
                              " needs at least one child: give new nodes as tree-JSON objects "
                              "and/or ids of existing nodes to move under it");
        }
    }
    if (parent_ref == nullptr) {
        return bad_params("parent: required: the id of the composite to add under, or null to "
                          "make the new node the root");
    }
    std::uint32_t parent = 0;
    if (!parent_ref->is_null()) {
        auto p = bt::read_node(parent_ref, g, "parent");
        if (!p) return std::unexpected(p.error());
        parent = *p;
    } else if (has_index) {
        return bad_params("index: not used with parent null (the root has no siblings)");
    }

    // Existing nodes to adopt are checked before the new node takes an id.
    std::vector<std::variant<std::uint32_t, const Json*>> adopt;
    for (std::size_t i = 0; has_children && i < children->size(); ++i) {
        const Json& c = (*children)[i];
        if (c.is_object()) {
            adopt.emplace_back(&c);
            continue;
        }
        auto id = bt::read_node(&c, g, "children[" + std::to_string(i) + "]");
        if (!id) return std::unexpected(id.error());
        adopt.emplace_back(*id);
    }

    const std::uint32_t old_root = scene::bt_graph_root(g);
    const std::uint32_t id = scene::bt_graph_add_node(g, *kind, leaf, {});
    if (has_params) {
        if (auto ok = merge_params(*g.find(id), *leaf_params, false); !ok) {
            return std::unexpected(ok.error());
        }
    }
    bool adopted_root = false;
    for (std::size_t i = 0; i < adopt.size(); ++i) {
        const std::string field = "children[" + std::to_string(i) + "]";
        std::uint32_t child = 0;
        if (const auto* existing = std::get_if<std::uint32_t>(&adopt[i])) {
            child = *existing;
            adopted_root = adopted_root || child == old_root;
        } else {
            auto grafted = graft(g, *std::get<const Json*>(adopt[i]), field);
            if (!grafted) return std::unexpected(grafted.error());
            child = *grafted;
        }
        if (auto ok = scene::bt_graph_attach(g, id, child); !ok) {
            return bad_params(field + ": " + ok.error().message);
        }
    }
    if (parent != 0) {
        if (has_index) {
            if (auto ok = check_index(g, parent, id, index); !ok) {
                return std::unexpected(ok.error());
            }
        }
        const std::size_t at = has_index ? static_cast<std::size_t>(index) : scene::bt_graph_append;
        if (auto ok = scene::bt_graph_attach(g, parent, id, at); !ok) {
            return bad_params("parent: " + ok.error().message);
        }
    } else if (old_root != 0 && !adopted_root) {
        return bad_params("parent: null makes the new node the root, so it must adopt the "
                          "current root (node " + std::to_string(old_root) +
                          ") in `children`; to add inside the tree, give a parent id");
    }
    if (!g.find(id)->children.empty()) scene::bt_graph_layout_subtree(g, id);
    const std::uint32_t placed[] = {id};
    auto view = commit_edit(ctx, *edit, placed, parent == 0);
    if (view) (*view)["node"] = id;
    return view;
}

RpcResult bt_remove_node(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto edit = open_edit(ctx, f);
    const Json* node_ref = f.raw("node");
    bool keep_children = false;
    f.boolean("keep_children", keep_children);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!edit) return std::unexpected(edit.error());
    BtGraph& g = edit->graph;
    auto id = bt::read_node(node_ref, g, "node");
    if (!id) return std::unexpected(id.error());

    const std::vector<std::uint32_t> children = g.find(*id)->children;
    const std::uint32_t parent = scene::bt_graph_parent(g, *id);
    if (parent == 0) {
        if (!keep_children) {
            return bad_params("node: " + std::to_string(*id) +
                              " is the root; replace the whole tree with bt.set, or remove the "
                              "\"behavior\" component with scene.remove_components");
        }
        if (children.size() != 1) {
            return bad_params("node: the root can only be removed with keep_children when it has "
                              "exactly one child (node " + std::to_string(*id) + " has " +
                              std::to_string(children.size()) + ")");
        }
    }
    std::vector<std::uint32_t> placed;
    if (keep_children && parent != 0) {
        // The children take the node's place among its siblings, in order.
        const auto& siblings = g.find(parent)->children;
        const auto at = static_cast<std::size_t>(std::ranges::find(siblings, *id) -
                                                 siblings.begin());
        (void)scene::bt_graph_unlink(g, parent, *id);
        for (std::size_t k = 0; k < children.size(); ++k) {
            if (auto ok = scene::bt_graph_attach(g, parent, children[k], at + k); !ok) {
                return bad_params("keep_children: node " + std::to_string(parent) +
                                  " cannot take the children: " + ok.error().message);
            }
        }
        placed = children;
        scene::bt_graph_remove_node(g, *id);
    } else if (keep_children) {
        scene::bt_graph_remove_node(g, *id); // the only child becomes the root
    } else {
        for (const std::uint32_t n : scene::bt_graph_subtree(g, *id)) {
            scene::bt_graph_remove_node(g, n);
        }
    }
    return commit_edit(ctx, *edit, placed, parent == 0);
}

RpcResult bt_move_node(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto edit = open_edit(ctx, f);
    const Json* node_ref = f.raw("node");
    const Json* parent_ref = f.raw("parent");
    int index = 0;
    const bool has_index = f.has("index");
    f.integer("index", index, 0, k_max_index);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!edit) return std::unexpected(edit.error());
    BtGraph& g = edit->graph;
    auto id = bt::read_node(node_ref, g, "node");
    if (!id) return std::unexpected(id.error());
    auto parent = bt::read_node(parent_ref, g, "parent");
    if (!parent) return std::unexpected(parent.error());
    if (scene::bt_graph_parent(g, *id) == 0) {
        return bad_params("node: " + std::to_string(*id) +
                          " is the root and cannot be moved; to put a new node above it, use "
                          "bt.add_node with parent null and the root among its children");
    }
    if (has_index) {
        if (auto ok = check_index(g, *parent, *id, index); !ok) return std::unexpected(ok.error());
    }
    const std::size_t at = has_index ? static_cast<std::size_t>(index) : scene::bt_graph_append;
    if (auto ok = scene::bt_graph_attach(g, *parent, *id, at); !ok) {
        return bad_params("parent: " + ok.error().message);
    }
    const std::uint32_t placed[] = {*id};
    return commit_edit(ctx, *edit, placed, false);
}

RpcResult bt_set_params(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto edit = open_edit(ctx, f);
    const Json* node_ref = f.raw("node");
    const Json* values = f.raw("params");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!edit) return std::unexpected(edit.error());
    auto id = bt::read_node(node_ref, edit->graph, "node");
    if (!id) return std::unexpected(id.error());
    BtGraphNode& node = *edit->graph.find(*id);
    if (node.kind != BtKind::leaf) {
        return bad_params("node: " + std::to_string(*id) + " is a " +
                          std::string(scene::BehaviorTreeAsset::kind_name(node.kind)) +
                          "; only leaves have parameters");
    }
    if (values == nullptr || values->is_null()) {
        return bad_params("params: required, e.g. {\"seconds\": 2} (null removes a parameter)");
    }
    if (auto ok = merge_params(node, *values, true); !ok) return std::unexpected(ok.error());
    return commit_edit(ctx, *edit, {}, false);
}

} // namespace

void register_behavior_edit_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("bt.add_node",
                      "Add a node to an entity's behaviour tree: {entity, parent (id | null), "
                      "index?, kind, leaf?, params?, children?} -> tree view + node.",
                      [ctx](const Json& p) { return bt_add_node(*ctx, p); });
    server.add_method("bt.remove_node",
                      "Remove a node with its subtree (or splice its children in with "
                      "keep_children): {entity, node, keep_children?} -> tree view.",
                      [ctx](const Json& p) { return bt_remove_node(*ctx, p); });
    server.add_method("bt.move_node",
                      "Re-parent or re-order a node with its subtree: {entity, node, parent, "
                      "index?} -> tree view.",
                      [ctx](const Json& p) { return bt_move_node(*ctx, p); });
    server.add_method("bt.set_params",
                      "Merge a leaf's parameters (null removes one): {entity, node, params} -> "
                      "tree view.",
                      [ctx](const Json& p) { return bt_set_params(*ctx, p); });
}

} // namespace engine::remote::world
