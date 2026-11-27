#include "engine/scene/bt_graph.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;
using Asset = BehaviorTreeAsset;

constexpr int           kLayoutVersion = 1;
constexpr std::uint32_t kMaxNodeId = 1U << 30; // keeps editor widget ids in range

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] const BtLeafInfo* find_leaf(BtLeafCatalog leaves, std::string_view name) {
    for (const BtLeafInfo& info : leaves) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

// "move_to" for leaves, the composite's type otherwise.
[[nodiscard]] std::string label(const BtGraphNode& n) {
    if (n.kind == BtKind::leaf) return n.leaf.empty() ? std::string("leaf") : n.leaf;
    return std::string(Asset::kind_name(n.kind));
}

[[nodiscard]] std::string trim_float(float f) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), f);
    return std::string(buf, r.ptr);
}

[[nodiscard]] std::string range_text(const BtParamInfo& p) {
    const bool lo = p.min > std::numeric_limits<float>::lowest();
    const bool hi = p.max < std::numeric_limits<float>::max();
    if (lo && hi) return "expected a number in [" + trim_float(p.min) + ", " + trim_float(p.max) + "]";
    if (lo) return "expected a number >= " + trim_float(p.min);
    if (hi) return "expected a number <= " + trim_float(p.max);
    return "expected a finite number";
}

// The float's shortest round-trip spelling as a double, so 0.1F is written as
// 0.1 rather than 0.10000000149011612.
[[nodiscard]] double json_number(float f) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf) - 1, f);
    *r.ptr = '\0';
    return std::strtod(buf, nullptr);
}

// --- JSON -> graph -----------------------------------------------------------

struct Reader {
    BtGraph&      graph;
    BtLeafCatalog leaves;

    std::expected<std::uint32_t, core::Error> read(const json& n, int depth,
                                                   const std::string& where) {
        if (depth > bt_max_depth) {
            return fail("behavior tree: nesting deeper than " +
                        std::to_string(bt_max_depth));
        }
        core::JsonFields f(n, where);
        if (depth == 0) (void)f.raw(bt_editor_field);
        std::string type;
        f.string("type", type, 64);
        if (!f.ok()) return std::unexpected(f.finish().error());
        if (type.empty()) return fail("behavior tree: " + where + " has no \"type\"");
        const std::optional<BtKind> kind = Asset::kind_from_name(type);
        if (!kind) {
            std::string known;
            for (const BtKindInfo& k : bt_kind_info()) {
                known += (known.empty() ? "" : ", ") + std::string(k.name);
            }
            return fail("behavior tree: " + where + ": unknown node type '" + type +
                        "' (known: " + known + ")");
        }

        const std::size_t index = graph.nodes.size();
        graph.nodes.emplace_back();
        graph.nodes[index].id = graph.next_id++;
        graph.nodes[index].kind = *kind;

        if (*kind == BtKind::leaf) {
            std::string name;
            f.string("name", name, 64);
            if (!f.ok()) return std::unexpected(f.finish().error());
            graph.nodes[index].leaf = std::move(name);
            // Every other field is a parameter; which ones the leaf knows is a
            // validation question, but each must be a number to be shown.
            for (const auto& [key, value] : n.items()) {
                if (key == "type" || key == "name") continue;
                if (depth == 0 && key == bt_editor_field) continue;
                if (key == "children") {
                    return fail("behavior tree: " + where + ": a leaf cannot have children");
                }
                float v = 0.0F;
                if (!core::JsonFields::read_float(value, v)) {
                    return fail("behavior tree: " + where + "." + key + ": expected a number");
                }
                graph.nodes[index].params.push_back({key, v});
            }
            // Parameters in the leaf's declared order (unknown ones last).
            if (const BtLeafInfo* info = find_leaf(leaves, graph.nodes[index].leaf)) {
                const auto rank = [info](const BtGraphParam& p) {
                    std::size_t i = 0;
                    while (i < info->params.size() && info->params[i].name != p.name) ++i;
                    return i;
                };
                std::ranges::stable_sort(graph.nodes[index].params, {}, rank);
            }
            return graph.nodes[index].id;
        }

        const json* children = f.raw("children");
        if (auto ok = f.finish(); !ok) return fail("behavior tree: " + ok.error().message);
        if (children != nullptr && !children->is_array()) {
            return fail("behavior tree: " + where + ".children: expected an array");
        }
        if (children != nullptr) {
            for (std::size_t i = 0; i < children->size(); ++i) {
                auto child = read((*children)[i], depth + 1,
                                  where + ".children[" + std::to_string(i) + "]");
                if (!child) return std::unexpected(child.error());
                graph.nodes[index].children.push_back(*child);
            }
        }
        return graph.nodes[index].id;
    }
};

// Applies the "editor" layout when it describes exactly this tree. Nodes were
// read in pre-order, which is the order the layout lists them in.
[[nodiscard]] bool apply_layout(BtGraph& graph, const json& root) {
    if (!root.is_object()) return false;
    const auto editor = root.find(bt_editor_field);
    if (editor == root.end() || !editor->is_object()) return false;
    const auto version = editor->find("version");
    std::int64_t v = 0;
    if (version == editor->end() || !core::JsonFields::read_int(*version, v) ||
        v != kLayoutVersion) {
        return false;
    }
    const auto nodes = editor->find("nodes");
    if (nodes == editor->end() || !nodes->is_array() || nodes->size() != graph.nodes.size()) {
        return false;
    }
    std::vector<std::uint32_t> ids;
    std::vector<glm::vec2>     positions;
    std::unordered_set<std::uint32_t> seen;
    for (const json& entry : *nodes) {
        std::int64_t id = 0;
        glm::vec2    p{0.0F};
        if (!entry.is_array() || entry.size() != 3 || !core::JsonFields::read_int(entry[0], id) ||
            id < 1 || id > kMaxNodeId || !core::JsonFields::read_float(entry[1], p.x) ||
            !core::JsonFields::read_float(entry[2], p.y) ||
            !seen.insert(static_cast<std::uint32_t>(id)).second) {
            return false;
        }
        ids.push_back(static_cast<std::uint32_t>(id));
        positions.push_back(p);
    }

    std::unordered_map<std::uint32_t, std::uint32_t> remap; // read id -> saved id
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) remap[graph.nodes[i].id] = ids[i];
    std::uint32_t max_id = 0;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        BtGraphNode& n = graph.nodes[i];
        n.id = ids[i];
        n.pos = positions[i];
        for (std::uint32_t& c : n.children) c = remap[c];
        max_id = std::max(max_id, n.id);
    }
    graph.next_id = max_id + 1;
    return true;
}

// --- graph -> JSON -----------------------------------------------------------

[[nodiscard]] nlohmann::ordered_json write_node(const BtGraph& graph, const BtGraphNode& n) {
    nlohmann::ordered_json o;
    o["type"] = std::string(Asset::kind_name(n.kind));
    if (n.kind == BtKind::leaf) {
        o["name"] = n.leaf;
        for (const BtGraphParam& p : n.params) o[p.name] = json_number(p.value);
        return o;
    }
    nlohmann::ordered_json children = nlohmann::ordered_json::array();
    for (const std::uint32_t c : n.children) children.push_back(write_node(graph, *graph.find(c)));
    o["children"] = std::move(children);
    return o;
}

// --- structure helpers ---------------------------------------------------------

[[nodiscard]] std::unordered_map<std::uint32_t, int> parent_counts(const BtGraph& graph) {
    std::unordered_map<std::uint32_t, int> counts;
    for (const BtGraphNode& n : graph.nodes) counts.emplace(n.id, 0);
    for (const BtGraphNode& n : graph.nodes) {
        for (const std::uint32_t c : n.children) {
            if (const auto it = counts.find(c); it != counts.end()) ++it->second;
        }
    }
    return counts;
}

// Pre-order walk from `root`, skipping nodes already visited (cycles end).
void walk(const BtGraph& graph, std::uint32_t id, std::unordered_set<std::uint32_t>& visited,
          std::vector<std::uint32_t>& out) {
    const BtGraphNode* n = graph.find(id);
    if (n == nullptr || !visited.insert(id).second) return;
    out.push_back(id);
    for (const std::uint32_t c : n->children) walk(graph, c, visited, out);
}

[[nodiscard]] std::vector<std::uint32_t> reachable(const BtGraph& graph, std::uint32_t root) {
    std::unordered_set<std::uint32_t> visited;
    std::vector<std::uint32_t>        out;
    walk(graph, root, visited, out);
    return out;
}

// Validation DFS: reports back edges (cycles) and excessive depth.
struct CycleCheck {
    const BtGraph&                         graph;
    std::unordered_map<std::uint32_t, int> state; // 1 on the stack, 2 done
    std::vector<BtGraphIssue>&             issues;
    bool                                   depth_reported = false;

    void visit(std::uint32_t id, int depth) {
        const BtGraphNode* n = graph.find(id);
        if (n == nullptr) return;
        if (depth > bt_max_depth && !depth_reported) {
            depth_reported = true;
            issues.push_back({id, label(*n) + ": nested deeper than " +
                                      std::to_string(bt_max_depth) + " levels"});
        }
        state[id] = 1;
        for (const std::uint32_t c : n->children) {
            const int s = state[c];
            if (s == 1) {
                issues.push_back({c, label(*graph.find(c)) + ": part of a cycle"});
            } else if (s == 0) {
                visit(c, depth + 1);
            }
        }
        state[id] = 2;
    }
};

void validate_node(const BtGraphNode& n, BtLeafCatalog leaves, std::vector<BtGraphIssue>& out) {
    const std::string who = label(n);
    if (n.kind != BtKind::leaf) {
        if (n.children.empty()) out.push_back({n.id, who + ": needs at least one child"});
        if (n.kind == BtKind::inverter && n.children.size() > 1) {
            out.push_back({n.id, who + ": takes exactly one child (has " +
                                     std::to_string(n.children.size()) + ")"});
        }
        if (!n.params.empty()) out.push_back({n.id, who + ": composites take no parameters"});
        return;
    }
    if (!n.children.empty()) out.push_back({n.id, who + ": a leaf cannot have children"});
    const BtLeafInfo* info = find_leaf(leaves, n.leaf);
    if (info == nullptr) {
        std::string known;
        for (const BtLeafInfo& l : leaves) {
            known += (known.empty() ? "" : ", ") + std::string(l.name);
        }
        out.push_back({n.id, "unknown leaf '" + n.leaf + "' (known: " + known + ")"});
        return;
    }
    for (std::size_t i = 0; i < n.params.size(); ++i) {
        const BtGraphParam& p = n.params[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (n.params[j].name == p.name) {
                out.push_back({n.id, who + ": parameter '" + p.name + "' set twice"});
            }
        }
        const BtParamInfo* pi = info->find_param(p.name);
        if (pi == nullptr) {
            std::string known;
            for (const BtParamInfo& k : info->params) {
                known += (known.empty() ? "" : ", ") + std::string(k.name);
            }
            out.push_back({n.id, who + ": unknown parameter '" + p.name + "'" +
                                     (known.empty() ? "" : " (known: " + known + ")")});
            continue;
        }
        if (!std::isfinite(p.value) || p.value < pi->min || p.value > pi->max) {
            out.push_back({n.id, who + "." + p.name + ": " + range_text(*pi)});
        }
    }
}

} // namespace

BtGraphNode* BtGraph::find(std::uint32_t id) {
    for (BtGraphNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const BtGraphNode* BtGraph::find(std::uint32_t id) const {
    for (const BtGraphNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

std::expected<BtGraph, core::Error> bt_graph_from_json(std::string_view json_text,
                                                       BtLeafCatalog leaves) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return fail("behavior tree: invalid JSON");
    BtGraph graph;
    Reader reader{graph, leaves};
    if (auto r = reader.read(root, 0, "root"); !r) return std::unexpected(r.error());
    if (!apply_layout(graph, root)) bt_graph_auto_layout(graph);
    return graph;
}

std::expected<std::string, core::Error> bt_graph_to_json(const BtGraph& graph,
                                                         BtLeafCatalog leaves) {
    const std::vector<BtGraphIssue> issues = bt_graph_validate(graph, leaves);
    if (!issues.empty()) {
        std::string msg = issues.front().message;
        if (issues.size() > 1) msg += " (and " + std::to_string(issues.size() - 1) + " more)";
        return fail(std::move(msg));
    }
    const std::uint32_t root = bt_graph_root(graph);
    std::string text = write_node(graph, *graph.find(root))
                           .dump(2, ' ', false, json::error_handler_t::replace);

    // Layout on one line after the tree, so the tree stays readable in the
    // inspector's text view.
    json layout = json::array();
    for (const std::uint32_t id : bt_graph_preorder(graph)) {
        const BtGraphNode& n = *graph.find(id);
        layout.push_back(json::array({id, std::lround(n.pos.x), std::lround(n.pos.y)}));
    }
    const json editor = {{"version", kLayoutVersion}, {"nodes", std::move(layout)}};
    // The pretty-printed object ends in "\n}".
    text.resize(text.size() - 2);
    text += ",\n  \"" + std::string(bt_editor_field) + "\": " + editor.dump() + "\n}";
    return text;
}

std::vector<BtGraphIssue> bt_graph_validate(const BtGraph& graph, BtLeafCatalog leaves) {
    std::vector<BtGraphIssue> issues;
    if (graph.nodes.empty()) {
        issues.push_back({0, "the tree is empty: add a node (right-click the canvas)"});
        return issues;
    }
    std::unordered_set<std::uint32_t> ids;
    for (const BtGraphNode& n : graph.nodes) {
        if (n.id == 0 || !ids.insert(n.id).second) {
            issues.push_back({0, "node id " + std::to_string(n.id) + " is not unique"});
        }
        for (const std::uint32_t c : n.children) {
            if (graph.find(c) == nullptr) {
                issues.push_back({n.id, label(n) + ": links to a missing node"});
            } else if (c == n.id) {
                issues.push_back({n.id, label(n) + ": linked to itself"});
            }
        }
    }

    const auto counts = parent_counts(graph);
    std::vector<std::uint32_t> roots;
    for (const BtGraphNode& n : graph.nodes) {
        const int c = counts.at(n.id);
        if (c == 0) roots.push_back(n.id);
        if (c > 1) {
            issues.push_back({n.id, label(n) + ": has " + std::to_string(c) +
                                        " parents (a node has exactly one)"});
        }
    }
    // The root is the parentless node heading the largest tree; other
    // parentless nodes are stray.
    std::uint32_t root = 0;
    std::size_t   best = 0;
    for (const std::uint32_t r : roots) {
        const std::size_t size = reachable(graph, r).size();
        if (size > best) {
            best = size;
            root = r;
        }
    }
    if (root == 0) {
        issues.push_back({0, "no root: every node has a parent, so the links form a cycle"});
    } else {
        CycleCheck check{graph, {}, issues};
        check.visit(root, 0);
    }
    const std::vector<std::uint32_t> tree = root != 0 ? reachable(graph, root)
                                                      : std::vector<std::uint32_t>{};
    const std::unordered_set<std::uint32_t> in_tree(tree.begin(), tree.end());
    for (const BtGraphNode& n : graph.nodes) {
        if (!in_tree.contains(n.id) && counts.at(n.id) <= 1) {
            issues.push_back({n.id, label(n) + ": not connected to the root '" +
                                        (root != 0 ? label(*graph.find(root)) : std::string("?")) +
                                        "'"});
        }
        validate_node(n, leaves, issues);
    }
    return issues;
}

std::uint32_t bt_graph_root(const BtGraph& graph) {
    std::uint32_t root = 0;
    for (const auto& [id, count] : parent_counts(graph)) {
        if (count != 0) continue;
        if (root != 0) return 0;
        root = id;
    }
    return root;
}

std::uint32_t bt_graph_parent(const BtGraph& graph, std::uint32_t child) {
    for (const BtGraphNode& n : graph.nodes) {
        if (std::ranges::find(n.children, child) != n.children.end()) return n.id;
    }
    return 0;
}

std::vector<std::uint32_t> bt_graph_preorder(const BtGraph& graph) {
    const std::uint32_t root = bt_graph_root(graph);
    return root != 0 ? reachable(graph, root) : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> bt_graph_subtree(const BtGraph& graph, std::uint32_t id) {
    return reachable(graph, id);
}

std::string bt_graph_path(const BtGraph& graph, std::uint32_t id) {
    const std::uint32_t root = bt_graph_root(graph);
    if (root == 0 || graph.find(id) == nullptr) return {};
    // Climb to the root, collecting each step's child index (bounded, so a
    // cycle above `id` ends the walk).
    std::vector<std::size_t> steps;
    std::uint32_t at = id;
    for (std::size_t guard = 0; at != root && guard <= graph.nodes.size(); ++guard) {
        const std::uint32_t parent = bt_graph_parent(graph, at);
        if (parent == 0) return {};
        const auto& siblings = graph.find(parent)->children;
        steps.push_back(static_cast<std::size_t>(std::ranges::find(siblings, at) -
                                                 siblings.begin()));
        at = parent;
    }
    if (at != root) return {};
    std::string path = "root";
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        path += ".children[" + std::to_string(*it) + "]";
    }
    return path;
}

} // namespace engine::scene
