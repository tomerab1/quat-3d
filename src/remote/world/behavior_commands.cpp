// bt.leaves, bt.get, bt.set, bt.status — behaviour-tree authoring (ROADMAP
// A7) on the graph model the editor's Behaviour panel uses
// (engine/scene/bt_graph.hpp), plus the helpers the edit commands share.

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "behavior_internal.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::remote::world {

namespace bt {

namespace {

constexpr std::size_t k_max_issues_in_message = 8;
constexpr std::size_t k_max_ids_listed = 32;
constexpr std::int64_t k_max_node_id = std::int64_t{1} << 30;

// The float's shortest round-trip spelling as a JSON number (0.1F -> 0.1).
[[nodiscard]] Json float_json(float f) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf) - 1, f);
    *r.ptr = '\0';
    return Json(std::strtod(buf, nullptr));
}

[[nodiscard]] Json node_json(const scene::BtGraphNode& n,
                             const std::unordered_map<std::uint32_t, std::string>& paths) {
    Json children = Json::array();
    for (const std::uint32_t c : n.children) children.push_back(c);
    Json out{{"id", n.id}, {"kind", std::string(scene::BehaviorTreeAsset::kind_name(n.kind))}};
    if (n.kind == scene::BtKind::leaf) {
        out["leaf"] = n.leaf;
        Json params = Json::object();
        for (const scene::BtGraphParam& p : n.params) params[p.name] = float_json(p.value);
        out["params"] = std::move(params);
    }
    out["children"] = std::move(children);
    const auto path = paths.find(n.id);
    out["path"] = path != paths.end() ? Json(path->second) : Json(nullptr);
    out["position"] = Json::array({float_json(n.pos.x), float_json(n.pos.y)});
    return out;
}

[[nodiscard]] std::string issue_text(const scene::BtGraphIssue& issue,
                                     const std::unordered_map<std::uint32_t, std::string>& paths) {
    if (issue.node == 0) return issue.message;
    std::string text = "node " + std::to_string(issue.node);
    if (const auto it = paths.find(issue.node); it != paths.end()) text += " at " + it->second;
    return text + ": " + issue.message;
}

[[nodiscard]] Json issues_json(const std::vector<scene::BtGraphIssue>& issues,
                               const std::unordered_map<std::uint32_t, std::string>& paths) {
    Json out = Json::array();
    for (const scene::BtGraphIssue& issue : issues) {
        Json i{{"node", issue.node}, {"message", issue.message}};
        if (const auto it = paths.find(issue.node); it != paths.end()) i["path"] = it->second;
        out.push_back(std::move(i));
    }
    return out;
}

// The stored source as a JSON value without the editor layout (null when
// empty or not JSON).
[[nodiscard]] Json tree_json(const std::string& source) {
    if (source.empty()) return nullptr;
    Json tree = Json::parse(source, nullptr, /*allow_exceptions=*/false);
    if (tree.is_discarded()) return nullptr;
    if (tree.is_object()) tree.erase(std::string(scene::bt_editor_field));
    return tree;
}

[[nodiscard]] std::string status_name(scene::BtNodeResult r) {
    switch (r) {
    case scene::BtNodeResult::success: return "success";
    case scene::BtNodeResult::failure: return "failure";
    case scene::BtNodeResult::running: return "running";
    case scene::BtNodeResult::idle: break;
    }
    return "idle";
}

} // namespace

std::expected<scene::BehaviorTree*, RpcError> tree_of(entt::registry& registry,
                                                      entt::entity entity) {
    auto* tree = registry.try_get<scene::BehaviorTree>(entity);
    if (tree == nullptr) {
        return rpc_fail(rpc_code::not_found,
                        "entity: entity " + std::to_string(entt::to_integral(entity)) +
                            " has no behaviour tree; create one with bt.set");
    }
    return tree;
}

std::expected<scene::BtGraph, RpcError> load_graph(const scene::BehaviorTree& tree) {
    if (tree.source.empty()) return scene::BtGraph{};
    auto graph = scene::bt_graph_from_json(tree.source);
    if (!graph) {
        return rpc_fail(rpc_code::failed,
                        "the stored tree cannot be edited as a graph (" +
                            rebase_error(graph.error().message, "root") +
                            "); replace it with bt.set");
    }
    return std::move(*graph);
}

Json tree_view(const entt::registry& registry, entt::entity entity) {
    const auto& tree = registry.get<scene::BehaviorTree>(entity);
    Json out{{"entity", entity_json(entity)},
             {"enabled", tree.enabled},
             {"compiled", tree.compiled != nullptr},
             {"parse_failed", tree.parse_failed}};
    Json nodes = Json::array();
    auto graph = load_graph(tree);
    if (!graph) {
        out["root"] = nullptr;
        out["nodes"] = std::move(nodes);
        out["issues"] = Json::array({Json{{"node", 0}, {"message", graph.error().message}}});
        out["tree"] = tree_json(tree.source);
        if (out["tree"].is_null()) out["source"] = tree.source;
        return out;
    }
    const auto paths = scene::bt_graph_paths(*graph);
    std::vector<std::uint32_t> order = scene::bt_graph_preorder(*graph);
    for (const scene::BtGraphNode& n : graph->nodes) {
        if (std::ranges::find(order, n.id) == order.end()) order.push_back(n.id);
    }
    for (const std::uint32_t id : order) nodes.push_back(node_json(*graph->find(id), paths));
    const std::uint32_t root = scene::bt_graph_root(*graph);
    out["root"] = root != 0 ? Json(root) : Json(nullptr);
    out["nodes"] = std::move(nodes);
    out["issues"] = graph->nodes.empty() ? Json::array()
                                         : issues_json(scene::bt_graph_validate(*graph), paths);
    out["tree"] = tree_json(tree.source);
    return out;
}

std::expected<std::string, RpcError> compile(const scene::BtGraph& graph) {
    const std::vector<scene::BtGraphIssue> issues = scene::bt_graph_validate(graph);
    if (!issues.empty()) {
        const auto paths = scene::bt_graph_paths(graph);
        std::string message = "invalid behaviour tree, nothing was changed: ";
        for (std::size_t i = 0; i < issues.size() && i < k_max_issues_in_message; ++i) {
            message += (i == 0 ? "" : "; ") + issue_text(issues[i], paths);
        }
        if (issues.size() > k_max_issues_in_message) {
            message += "; and " + std::to_string(issues.size() - k_max_issues_in_message) +
                       " more (see data.issues)";
        }
        return std::unexpected(RpcError{rpc_code::invalid_params, std::move(message),
                                        Json{{"issues", issues_json(issues, paths)}}});
    }
    auto text = scene::bt_graph_to_json(graph);
    if (!text) return rpc_fail(rpc_code::failed, text.error().message);
    // The runtime is the authority; validation should agree with it.
    if (auto compiled = scene::BehaviorTreeAsset::parse(*text); !compiled) {
        return bad_params("the runtime rejects the tree, nothing was changed: " +
                          rebase_error(compiled.error().message, "root"));
    }
    return std::move(*text);
}

void write_source(scene::BehaviorTree& tree, std::string text) {
    tree.source = std::move(text);
    tree.compiled.reset(); // recompiled lazily by bt_system
    tree.node_state.clear();
    tree.last_result.clear();
    tree.parse_failed = false;
}

std::expected<std::uint32_t, RpcError> read_node(const Json* value, const scene::BtGraph& graph,
                                                 std::string_view field) {
    const std::string f(field);
    if (value == nullptr || value->is_null()) {
        return bad_params(f + ": required (a node id, as listed by bt.get)");
    }
    std::int64_t id = 0;
    if (!core::JsonFields::read_int(*value, id) || id < 1 || id > k_max_node_id) {
        return bad_params(f + ": expected a node id (a positive integer, as listed by bt.get)");
    }
    if (graph.find(static_cast<std::uint32_t>(id)) == nullptr) {
        std::vector<std::uint32_t> ids;
        for (const scene::BtGraphNode& n : graph.nodes) ids.push_back(n.id);
        std::ranges::sort(ids);
        std::string known;
        for (std::size_t i = 0; i < ids.size() && i < k_max_ids_listed; ++i) {
            known += (i == 0 ? "" : ", ") + std::to_string(ids[i]);
        }
        if (ids.size() > k_max_ids_listed) known += ", ...";
        return rpc_fail(rpc_code::not_found,
                        f + ": no node " + std::to_string(id) + " in this tree" +
                            (ids.empty() ? std::string(" (it is empty)")
                                         : " (its nodes: " + known + ")"));
    }
    return static_cast<std::uint32_t>(id);
}

std::string rebase_error(std::string message, std::string_view where) {
    constexpr std::string_view prefix = "behavior tree: ";
    if (message.starts_with(prefix)) message.erase(0, prefix.size());
    if (message.starts_with("root")) return std::string(where) + message.substr(4);
    return std::string(where) + ": " + message;
}

std::string known_kinds() {
    std::string known;
    for (const scene::BtKindInfo& k : scene::bt_kind_info()) {
        known += (known.empty() ? "" : ", ") + std::string(k.name);
    }
    return known;
}

std::string known_leaves() {
    std::string known;
    for (const scene::BtLeafInfo& l : scene::bt_builtin_leaf_info()) {
        known += (known.empty() ? "" : ", ") + std::string(l.name);
    }
    return known;
}

} // namespace bt

namespace {

RpcResult bt_leaves(const Json&) {
    Json kinds = Json::array();
    for (const scene::BtKindInfo& k : scene::bt_kind_info()) {
        kinds.push_back({{"name", std::string(k.name)},
                         {"min_children", k.min_children},
                         {"max_children",
                          k.max_children < 0 ? Json(nullptr) : Json(k.max_children)},
                         {"help", std::string(k.help)}});
    }
    Json leaves = Json::array();
    for (const scene::BtLeafInfo& l : scene::bt_builtin_leaf_info()) {
        Json params = Json::array();
        for (const scene::BtParamInfo& p : l.params) {
            Json param{{"name", std::string(p.name)},
                       {"type", "number"},
                       {"default", bt::float_json(p.default_value)},
                       {"optional", p.optional},
                       {"help", std::string(p.help)}};
            if (p.min > std::numeric_limits<float>::lowest()) param["min"] = bt::float_json(p.min);
            if (p.max < std::numeric_limits<float>::max()) param["max"] = bt::float_json(p.max);
            params.push_back(std::move(param));
        }
        leaves.push_back({{"name", std::string(l.name)},
                          {"help", std::string(l.help)},
                          {"params", std::move(params)}});
    }
    return Json{{"kinds", std::move(kinds)}, {"leaves", std::move(leaves)}};
}

RpcResult bt_get(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    if (auto tree = bt::tree_of(r, *e); !tree) return std::unexpected(tree.error());
    return bt::tree_view(r, *e);
}

RpcResult bt_set(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    const Json* given = f.raw("tree");
    bool enabled = true;
    const bool has_enabled = f.has("enabled");
    f.boolean("enabled", enabled);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    if (given == nullptr || given->is_null()) {
        return bad_params("tree: required: the behaviour tree as an object, e.g. {\"type\": "
                          "\"sequence\", \"children\": [...]} (bt.leaves lists the kinds and "
                          "leaves)");
    }
    Json doc;
    if (given->is_string()) {
        doc = Json::parse(given->get_ref<const std::string&>(), nullptr, false);
        if (doc.is_discarded()) return bad_params("tree: the string is not valid JSON");
    } else if (given->is_object()) {
        doc = *given;
    } else {
        return bad_params("tree: expected a behaviour tree object (or its JSON text)");
    }
    auto graph =
        scene::bt_graph_from_json(doc.dump(-1, ' ', false, Json::error_handler_t::replace));
    if (!graph) return bad_params("tree: " + bt::rebase_error(graph.error().message, "root"));

    // Without its own layout, a tree of the current shape keeps the current
    // ids and positions (e.g. a parameter tweak).
    auto* existing = r.try_get<scene::BehaviorTree>(*e);
    if (existing != nullptr && !doc.contains(std::string(scene::bt_editor_field))) {
        if (auto previous = bt::load_graph(*existing); previous && !previous->nodes.empty()) {
            (void)scene::bt_graph_reuse_layout(*graph, *previous);
        }
    }
    auto text = bt::compile(*graph);
    if (!text) return std::unexpected(text.error());
    auto& tree = r.get_or_emplace<scene::BehaviorTree>(*e);
    bt::write_source(tree, std::move(*text));
    if (has_enabled) tree.enabled = enabled;
    return bt::tree_view(r, *e);
}

RpcResult bt_status(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    auto tree = bt::tree_of(r, *e);
    if (!tree) return std::unexpected(tree.error());
    const scene::BehaviorTree& t = **tree;

    Json nodes = Json::array();
    Json root_status = nullptr;
    const bool ticked = t.compiled != nullptr && !t.last_result.empty() &&
                        t.last_result.size() == t.compiled->nodes().size();
    if (ticked) {
        // The runtime compiles nodes in the graph's pre-order.
        auto graph = bt::load_graph(t);
        const std::vector<std::uint32_t> order =
            graph ? scene::bt_graph_preorder(*graph) : std::vector<std::uint32_t>{};
        const auto paths = graph ? scene::bt_graph_paths(*graph)
                                 : std::unordered_map<std::uint32_t, std::string>{};
        const bool mapped = order.size() == t.last_result.size();
        for (std::size_t i = 0; i < t.last_result.size(); ++i) {
            const auto path = mapped ? paths.find(order[i]) : paths.end();
            Json n{{"id", mapped ? Json(order[i]) : Json(nullptr)},
                   {"path", path != paths.end() ? Json(path->second) : Json(nullptr)},
                   {"status", bt::status_name(t.last_result[i])}};
            nodes.push_back(std::move(n));
        }
        root_status = bt::status_name(t.last_result.front());
    }
    return Json{{"entity", entity_json(*e)},
                {"playing", ctx.services.playing ? Json(ctx.services.playing()) : Json(nullptr)},
                {"enabled", t.enabled},
                {"compiled", t.compiled != nullptr},
                {"tick", t.tick_count},
                {"root_status", std::move(root_status)},
                {"nodes", std::move(nodes)}};
}

} // namespace

void register_behavior_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("bt.leaves",
                      "Behaviour-tree node kinds and leaves with their parameters: -> {kinds, "
                      "leaves:[{name, help, params:[{name, type, default, min?, max?, optional, "
                      "help}]}]}.",
                      bt_leaves);
    server.add_method("bt.get",
                      "An entity's behaviour tree: {entity} -> {entity, enabled, compiled, root, "
                      "nodes:[{id, kind, leaf?, params?, children, path, position}], issues, "
                      "tree}.",
                      [ctx](const Json& p) { return bt_get(*ctx, p); });
    server.add_method("bt.set",
                      "Replace an entity's behaviour tree (validated; creates the component): "
                      "{entity, tree, enabled?} -> tree view.",
                      [ctx](const Json& p) { return bt_set(*ctx, p); });
    server.add_method("bt.status",
                      "What each node returned on the tree's last tick (Play): {entity} -> "
                      "{playing, compiled, tick, root_status, nodes:[{id, path, status}]}.",
                      [ctx](const Json& p) { return bt_status(*ctx, p); });
}

} // namespace engine::remote::world
