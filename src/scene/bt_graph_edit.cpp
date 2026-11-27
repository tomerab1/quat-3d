// Behaviour-tree graph edits and layout (see engine/scene/bt_graph.hpp); the
// JSON conversion, validation and queries are in bt_graph.cpp.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "engine/scene/bt_graph.hpp"

namespace engine::scene {

namespace {

constexpr float kLayoutSpacingX = 250.0F;
constexpr float kLayoutSpacingY = 190.0F;
// The saved layout rounds positions to whole units.
constexpr float kLayoutTolerance = 1.0F;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] const BtLeafInfo* find_leaf(BtLeafCatalog leaves, std::string_view name) {
    for (const BtLeafInfo& info : leaves) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

// Translates `id` and its subtree by `delta`.
void shift_subtree(BtGraph& graph, std::uint32_t id, glm::vec2 delta) {
    for (const std::uint32_t n : bt_graph_subtree(graph, id)) graph.find(n)->pos += delta;
}

// Tidy top-down layout of `root`'s subtree with `root` at the origin: leaves
// take consecutive columns, parents centre over their children.
void layout_from(BtGraph& graph, std::uint32_t root) {
    std::unordered_set<std::uint32_t> placed;
    float next_x = 0.0F;
    const auto place = [&](const auto& self, std::uint32_t id, int depth) -> float {
        BtGraphNode* n = graph.find(id);
        if (n == nullptr || !placed.insert(id).second) return next_x;
        std::vector<float> xs;
        for (const std::uint32_t c : n->children) {
            if (!placed.contains(c) && graph.find(c) != nullptr) xs.push_back(self(self, c, depth + 1));
        }
        if (xs.empty()) {
            n->pos.x = next_x;
            next_x += kLayoutSpacingX;
        } else {
            n->pos.x = 0.5F * (xs.front() + xs.back());
        }
        n->pos.y = static_cast<float>(depth) * kLayoutSpacingY;
        return n->pos.x;
    };
    place(place, root, 0);
}

} // namespace

std::uint32_t bt_graph_add_node(BtGraph& graph, BtKind kind, std::string_view leaf,
                                glm::vec2 pos, BtLeafCatalog leaves) {
    BtGraphNode n;
    n.id = graph.next_id++;
    n.kind = kind;
    n.pos = pos;
    if (kind == BtKind::leaf) {
        n.leaf = std::string(leaf);
        if (const BtLeafInfo* info = find_leaf(leaves, leaf)) {
            for (const BtParamInfo& p : info->params) {
                if (!p.optional) n.params.push_back({std::string(p.name), p.default_value});
            }
        }
    }
    graph.nodes.push_back(std::move(n));
    return graph.nodes.back().id;
}

void bt_graph_remove_node(BtGraph& graph, std::uint32_t id) {
    std::erase_if(graph.nodes, [id](const BtGraphNode& n) { return n.id == id; });
    for (BtGraphNode& n : graph.nodes) std::erase(n.children, id);
}

std::expected<void, core::Error> bt_graph_can_link(const BtGraph& graph, std::uint32_t parent,
                                                   std::uint32_t child) {
    const BtGraphNode* p = graph.find(parent);
    const BtGraphNode* c = graph.find(child);
    if (p == nullptr || c == nullptr) return fail("no such node");
    if (parent == child) return fail("a node cannot be its own child");
    if (p->kind == BtKind::leaf) return fail("a leaf cannot have children");
    if (std::ranges::find(p->children, child) != p->children.end()) return fail("already linked");
    if (p->kind == BtKind::inverter && !p->children.empty()) {
        return fail("an inverter takes exactly one child");
    }
    // Linking under one of child's own descendants would close a cycle.
    for (const std::uint32_t d : bt_graph_subtree(graph, child)) {
        if (d == parent) return fail("would create a cycle");
    }
    return {};
}

std::expected<void, core::Error> bt_graph_link(BtGraph& graph, std::uint32_t parent,
                                               std::uint32_t child) {
    if (auto ok = bt_graph_can_link(graph, parent, child); !ok) return ok;
    for (BtGraphNode& n : graph.nodes) std::erase(n.children, child);
    graph.find(parent)->children.push_back(child);
    return {};
}

bool bt_graph_unlink(BtGraph& graph, std::uint32_t parent, std::uint32_t child) {
    BtGraphNode* p = graph.find(parent);
    return p != nullptr && std::erase(p->children, child) > 0;
}

bool bt_graph_move_child(BtGraph& graph, std::uint32_t child, int delta) {
    BtGraphNode* p = graph.find(bt_graph_parent(graph, child));
    if (p == nullptr || delta == 0) return false;
    const auto it = std::ranges::find(p->children, child);
    const auto i = static_cast<std::ptrdiff_t>(it - p->children.begin());
    const std::ptrdiff_t j = i + (delta < 0 ? -1 : 1);
    if (j < 0 || j >= static_cast<std::ptrdiff_t>(p->children.size())) return false;
    const std::uint32_t other = p->children[static_cast<std::size_t>(j)];
    std::swap(p->children[static_cast<std::size_t>(i)], p->children[static_cast<std::size_t>(j)]);
    // Swap the two subtrees on screen too, so sorting by x keeps this order.
    const float dx = graph.find(other)->pos.x - graph.find(child)->pos.x;
    shift_subtree(graph, child, {dx, 0.0F});
    shift_subtree(graph, other, {-dx, 0.0F});
    return true;
}

bool bt_graph_sort_children_by_x(BtGraph& graph) {
    bool changed = false;
    for (BtGraphNode& n : graph.nodes) {
        if (n.children.size() < 2) continue;
        std::vector<std::uint32_t> sorted = n.children;
        std::ranges::stable_sort(sorted, [&](std::uint32_t a, std::uint32_t b) {
            const BtGraphNode* na = graph.find(a);
            const BtGraphNode* nb = graph.find(b);
            return na != nullptr && nb != nullptr && na->pos.x < nb->pos.x;
        });
        if (sorted != n.children) {
            n.children = std::move(sorted);
            changed = true;
        }
    }
    return changed;
}

void bt_graph_auto_layout(BtGraph& graph) {
    std::uint32_t root = bt_graph_root(graph);
    if (root == 0 && !graph.nodes.empty()) root = graph.nodes.front().id;
    if (root != 0) layout_from(graph, root);
}

void bt_graph_layout_subtree(BtGraph& graph, std::uint32_t id) {
    const BtGraphNode* n = graph.find(id);
    if (n == nullptr) return;
    const glm::vec2 anchor = n->pos;
    layout_from(graph, id);
    shift_subtree(graph, id, anchor - graph.find(id)->pos);
}

std::expected<void, core::Error> bt_graph_attach(BtGraph& graph, std::uint32_t parent,
                                                 std::uint32_t child, std::size_t index) {
    BtGraphNode* p = graph.find(parent);
    if (p == nullptr || graph.find(child) == nullptr) return fail("no such node");
    const bool reorder = std::ranges::find(p->children, child) != p->children.end();
    if (!reorder) {
        if (auto ok = bt_graph_can_link(graph, parent, child); !ok) return ok;
    }
    const std::size_t others = p->children.size() - (reorder ? 1U : 0U);
    if (index == bt_graph_append) index = others;
    if (index > others) {
        return fail("index " + std::to_string(index) + " is past the end (expected 0.." +
                    std::to_string(others) + ")");
    }
    for (BtGraphNode& n : graph.nodes) std::erase(n.children, child);
    p->children.insert(p->children.begin() + static_cast<std::ptrdiff_t>(index), child);
    return {};
}

bool bt_graph_has_auto_layout(const BtGraph& graph) {
    BtGraph tidy = graph;
    bt_graph_auto_layout(tidy);
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const glm::vec2 d = glm::abs(graph.nodes[i].pos - tidy.nodes[i].pos);
        if (d.x > kLayoutTolerance || d.y > kLayoutTolerance) return false;
    }
    return true;
}

void bt_graph_place_subtree(BtGraph& graph, std::uint32_t id) {
    const BtGraphNode* n = graph.find(id);
    const BtGraphNode* p = graph.find(bt_graph_parent(graph, id));
    if (n == nullptr || p == nullptr) return;
    const std::vector<std::uint32_t>& siblings = p->children;
    const auto i = static_cast<std::size_t>(std::ranges::find(siblings, id) - siblings.begin());
    const BtGraphNode* left = i > 0 ? graph.find(siblings[i - 1]) : nullptr;
    const BtGraphNode* right = i + 1 < siblings.size() ? graph.find(siblings[i + 1]) : nullptr;
    glm::vec2 target{p->pos.x, p->pos.y + kLayoutSpacingY};
    if (left != nullptr && right != nullptr) {
        target.x = 0.5F * (left->pos.x + right->pos.x);
    } else if (left != nullptr) {
        target.x = left->pos.x + kLayoutSpacingX;
    } else if (right != nullptr) {
        target.x = right->pos.x - kLayoutSpacingX;
    }
    shift_subtree(graph, id, target - n->pos);
}

bool bt_graph_reuse_layout(BtGraph& graph, const BtGraph& previous) {
    const std::vector<std::uint32_t> now = bt_graph_preorder(graph);
    const std::vector<std::uint32_t> before = bt_graph_preorder(previous);
    if (now.empty() || now.size() != before.size() || now.size() != graph.nodes.size()) {
        return false;
    }
    std::unordered_map<std::uint32_t, std::uint32_t> remap; // id now -> id before
    for (std::size_t i = 0; i < now.size(); ++i) {
        const BtGraphNode& a = *graph.find(now[i]);
        const BtGraphNode& b = *previous.find(before[i]);
        if (a.kind != b.kind || a.leaf != b.leaf || a.children.size() != b.children.size()) {
            return false;
        }
        remap[now[i]] = before[i];
    }
    for (BtGraphNode& n : graph.nodes) {
        n.id = remap[n.id];
        n.pos = previous.find(n.id)->pos;
        for (std::uint32_t& c : n.children) c = remap[c];
    }
    graph.next_id = previous.next_id;
    for (const BtGraphNode& n : graph.nodes) graph.next_id = std::max(graph.next_id, n.id + 1);
    return true;
}

bool bt_graph_children_follow_x(const BtGraph& graph) {
    BtGraph sorted = graph;
    return !bt_graph_sort_children_by_x(sorted);
}

} // namespace engine::scene
