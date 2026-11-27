// Behaviour panel: the node canvas (nodes, pins, links, link creation and
// deletion, context menus) and the read-back of dragged node positions.
// Split from behavior_panel.cpp, which owns loading and committing.

#include "behavior_panel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <imgui.h>
// vcpkg installs the header under include/imgui-node-editor/; a plain source
// checkout (local compile checks) has it at the repository root.
#if __has_include(<imgui-node-editor/imgui_node_editor.h>)
#include <imgui-node-editor/imgui_node_editor.h>
#else
#include <imgui_node_editor.h>
#endif

#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "behavior_panel_style.hpp"

namespace engine::editor {

namespace {

namespace ned = ax::NodeEditor;

// Widget ids: a node's id shifted left two bits, the low bits telling the
// node, its top (input) pin, its bottom (output) pin and the link into it
// apart. Graph ids stay below 2^30 (bt_graph.cpp), so nothing overflows.
enum : std::uintptr_t { kTagNode = 0, kTagIn = 1, kTagOut = 2, kTagLink = 3 };

[[nodiscard]] std::uintptr_t widget_id(std::uint32_t id, std::uintptr_t tag) {
    return (static_cast<std::uintptr_t>(id) << 2U) | tag;
}
[[nodiscard]] ned::NodeId node_id(std::uint32_t id) { return {widget_id(id, kTagNode)}; }
[[nodiscard]] ned::PinId in_pin(std::uint32_t id) { return {widget_id(id, kTagIn)}; }
[[nodiscard]] ned::PinId out_pin(std::uint32_t id) { return {widget_id(id, kTagOut)}; }
[[nodiscard]] ned::LinkId link_id(std::uint32_t child) { return {widget_id(child, kTagLink)}; }
[[nodiscard]] std::uint32_t graph_id(std::uintptr_t widget) {
    return static_cast<std::uint32_t>(widget >> 2U);
}
[[nodiscard]] std::uintptr_t widget_tag(std::uintptr_t widget) { return widget & 3U; }

constexpr float kCompositeWidth = 150.0F;
constexpr float kLeafWidth = 190.0F;
constexpr float kPinHeight = 8.0F;


constexpr BtKind kComposites[] = {BtKind::sequence, BtKind::selector, BtKind::reactive_sequence,
                                  BtKind::reactive_selector, BtKind::inverter};

[[nodiscard]] const char* display_name(BtKind kind) {
    switch (kind) {
    case BtKind::sequence: return "Sequence";
    case BtKind::selector: return "Selector";
    case BtKind::reactive_sequence: return "Reactive sequence";
    case BtKind::reactive_selector: return "Reactive selector";
    case BtKind::inverter: return "Inverter";
    case BtKind::leaf: break;
    }
    return "Leaf";
}

[[nodiscard]] const char* kind_help(BtKind kind) {
    switch (kind) {
    case BtKind::sequence:
        return "Runs children left to right until one fails; resumes at the running child.";
    case BtKind::selector:
        return "Tries children left to right until one does not fail; resumes at the running "
               "child.";
    case BtKind::reactive_sequence:
        return "Re-checks every child from the left each tick: a condition that stops holding "
               "aborts the running child.";
    case BtKind::reactive_selector:
        return "Re-evaluates higher-priority (left) children each tick; one that succeeds or "
               "runs pre-empts the running one.";
    case BtKind::inverter: return "Swaps success and failure of its single child.";
    case BtKind::leaf: break;
    }
    return "";
}

[[nodiscard]] ImVec4 kind_color(BtKind kind) {
    switch (kind) {
    case BtKind::sequence: return {0.40F, 0.62F, 0.98F, 1.0F};
    case BtKind::selector: return {0.98F, 0.66F, 0.30F, 1.0F};
    case BtKind::reactive_sequence: return {0.45F, 0.82F, 0.98F, 1.0F};
    case BtKind::reactive_selector: return {0.98F, 0.82F, 0.35F, 1.0F};
    case BtKind::inverter: return {0.78F, 0.55F, 0.98F, 1.0F};
    case BtKind::leaf: break;
    }
    return {0.55F, 0.85F, 0.70F, 1.0F};
}

[[nodiscard]] std::string range_hint(const scene::BtParamInfo& p) {
    const bool lo = p.min > std::numeric_limits<float>::lowest();
    const bool hi = p.max < std::numeric_limits<float>::max();
    char buf[96] = "";
    if (lo && hi) {
        std::snprintf(buf, sizeof(buf), "range [%g, %g], ", static_cast<double>(p.min),
                      static_cast<double>(p.max));
    } else if (lo) {
        std::snprintf(buf, sizeof(buf), ">= %g, ", static_cast<double>(p.min));
    }
    char def[48];
    std::snprintf(def, sizeof(def), "default %g", static_cast<double>(p.default_value));
    return std::string(buf) + def;
}

// Sets (or adds, in the leaf's declared order) parameter `name`.
void set_param(BtGraphNode& node, const scene::BtLeafInfo& info, std::string_view name,
               float value) {
    for (BtGraphParam& p : node.params) {
        if (p.name == name) {
            p.value = value;
            return;
        }
    }
    const auto rank = [&](std::string_view n) {
        std::size_t i = 0;
        while (i < info.params.size() && info.params[i].name != n) ++i;
        return i;
    };
    const auto at = std::ranges::find_if(node.params, [&](const BtGraphParam& p) {
        return rank(p.name) > rank(name);
    });
    node.params.insert(at, BtGraphParam{std::string(name), value});
}

// Small bar marking a pin, centred in the pin's rect.
void draw_pin_glyph(bool linked) {
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const ImVec2 c(0.5F * (lo.x + hi.x), 0.5F * (lo.y + hi.y));
    const ImU32 col = linked ? IM_COL32(200, 205, 215, 255) : IM_COL32(120, 125, 135, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(c.x - 16.0F, c.y - 2.5F),
                                              ImVec2(c.x + 16.0F, c.y + 2.5F), col, 2.0F);
}

} // namespace

void BehaviorPanel::draw_canvas(const scene::BehaviorTree& bt, float height) {
    ned::Begin("bt_canvas", ImVec2(0.0F, height));

    if (push_all_positions_) {
        for (const BtGraphNode& n : graph_.nodes) {
            ned::SetNodePosition(node_id(n.id), ImVec2(n.pos.x, n.pos.y));
        }
        ned::ClearSelection();
    } else {
        for (const std::uint32_t id : push_positions_) {
            if (const BtGraphNode* n = graph_.find(id)) {
                ned::SetNodePosition(node_id(id), ImVec2(n->pos.x, n->pos.y));
            }
        }
    }
    push_all_positions_ = false;
    push_positions_.clear();

    // Running nodes: the compiled tree stores nodes in the graph's pre-order.
    std::unordered_set<std::uint32_t> running;
    if (!unapplied_ && bt.compiled != nullptr) {
        const std::vector<std::uint32_t> order = bt_graph_preorder(graph_);
        if (order.size() == bt.compiled->nodes().size() && order.size() == bt.node_state.size()) {
            for (std::size_t i = 0; i < order.size(); ++i) {
                const scene::BtNodeState& s = bt.node_state[i];
                if (s.running && s.ticked == bt.tick_count) running.insert(order[i]);
            }
        }
    }
    std::unordered_set<std::uint32_t> flagged;
    for (const BtGraphIssue& i : issues_) flagged.insert(i.node);
    std::unordered_map<std::uint32_t, int> order;
    for (const BtGraphNode& n : graph_.nodes) {
        for (std::size_t k = 0; n.children.size() > 1 && k < n.children.size(); ++k) {
            order[n.children[k]] = static_cast<int>(k) + 1;
        }
    }

    for (BtGraphNode& n : graph_.nodes) {
        const auto it = order.find(n.id);
        draw_node(n, running.contains(n.id), flagged.contains(n.id),
                  it != order.end() ? it->second : 0);
    }
    for (const BtGraphNode& n : graph_.nodes) {
        for (const std::uint32_t c : n.children) {
            const bool hot = running.contains(c);
            ned::Link(link_id(c), out_pin(n.id), in_pin(c),
                      hot ? kRunningColor : ImVec4(0.75F, 0.77F, 0.80F, 1.0F), hot ? 3.0F : 2.0F);
        }
    }

    handle_create();
    handle_delete();

    if (navigate_in_frames_ > 0 && --navigate_in_frames_ == 0) ned::NavigateToContent(0.0F);
    if (focus_node_ != 0) {
        ned::SelectNode(node_id(focus_node_));
        ned::NavigateToSelection(false);
        focus_node_ = 0;
    }
    draw_context_menus();
    ned::End();

    if (graph_.nodes.empty()) {
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        const char* hint = "Empty tree: right-click to add a node";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(0.5F * (lo.x + hi.x - size.x), 0.5F * (lo.y + hi.y - size.y)),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }
    if (!tooltip_.empty()) {
        ImGui::SetTooltip("%s", tooltip_.c_str());
        tooltip_.clear();
    }
    read_back_positions();
}

void BehaviorPanel::draw_node(BtGraphNode& node, bool running, bool has_issue, int order) {
    const ImVec4 accent = kind_color(node.kind);
    const ImVec4 border = running     ? kRunningColor
                          : has_issue ? kErrorColor
                                      : ImVec4(accent.x, accent.y, accent.z, 0.55F);
    ned::PushStyleColor(ned::StyleColor_NodeBorder, border);
    ned::PushStyleVar(ned::StyleVar_NodeBorderWidth, running || has_issue ? 3.0F : 1.5F);
    ned::BeginNode(node_id(node.id));
    ImGui::PushID(static_cast<int>(node.id));
    const bool leaf = node.kind == BtKind::leaf;
    const float width = leaf ? kLeafWidth : kCompositeWidth;

    // Top pin: the link from the parent.
    ned::BeginPin(in_pin(node.id), ned::PinKind::Input);
    ned::PinPivotAlignment(ImVec2(0.5F, 0.0F));
    ned::PinPivotSize(ImVec2(0.0F, 0.0F));
    ImGui::Dummy(ImVec2(width, kPinHeight));
    draw_pin_glyph(ned::HasAnyLinks(in_pin(node.id)));
    ned::EndPin();

    ImGui::TextColored(accent, "%s", leaf ? node.leaf.c_str() : display_name(node.kind));
    if (ImGui::IsItemHovered()) {
        if (leaf) {
            const scene::BtLeafInfo* info = scene::bt_find_leaf_info(node.leaf);
            tooltip_ = info != nullptr ? std::string(info->help) : "unknown leaf";
        } else {
            tooltip_ = kind_help(node.kind);
        }
    }
    if (order > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("#%d", order);
    }
    if (running) {
        ImGui::SameLine();
        ImGui::TextColored(kRunningColor, "running");
    }

    if (leaf) {
        draw_leaf_params(node);
    } else {
        ned::BeginPin(out_pin(node.id), ned::PinKind::Output);
        ned::PinPivotAlignment(ImVec2(0.5F, 1.0F));
        ned::PinPivotSize(ImVec2(0.0F, 0.0F));
        ImGui::Dummy(ImVec2(width, kPinHeight));
        draw_pin_glyph(!node.children.empty());
        ned::EndPin();
    }
    ImGui::PopID();
    ned::EndNode();
    ned::PopStyleVar();
    ned::PopStyleColor();
}

void BehaviorPanel::draw_leaf_params(BtGraphNode& node) {
    const float label_room = 70.0F;
    ImGui::PushItemWidth(kLeafWidth - label_room);
    const scene::BtLeafInfo* info = scene::bt_find_leaf_info(node.leaf);
    if (info == nullptr) {
        ImGui::TextColored(kErrorColor, "unknown leaf");
    } else {
        for (const scene::BtParamInfo& p : info->params) {
            const std::string name(p.name);
            const auto it = std::ranges::find(node.params, name, &BtGraphParam::name);
            bool present = it != node.params.end();
            if (p.optional) {
                if (ImGui::Checkbox(("##set_" + name).c_str(), &present)) {
                    if (present) {
                        set_param(node, *info, p.name, p.default_value);
                    } else {
                        std::erase_if(node.params, [&](const BtGraphParam& q) { return q.name == name; });
                    }
                    note(Change::structure);
                }
                if (ImGui::IsItemHovered()) tooltip_ = std::string(p.help);
                ImGui::SameLine();
                if (!present) {
                    ImGui::TextDisabled("%s (unset)", name.c_str());
                    continue;
                }
            }
            float v = present ? std::ranges::find(node.params, name, &BtGraphParam::name)->value
                              : p.default_value;
            const bool unbounded = p.min == std::numeric_limits<float>::lowest() &&
                                   p.max == std::numeric_limits<float>::max();
            if (p.optional) ImGui::SetNextItemWidth(kLeafWidth - label_room - 28.0F);
            if (ImGui::DragFloat(name.c_str(), &v, 0.05F, unbounded ? 0.0F : p.min,
                                 unbounded ? 0.0F : p.max, "%.2f",
                                 unbounded ? ImGuiSliderFlags_None : ImGuiSliderFlags_AlwaysClamp)) {
                set_param(node, *info, p.name, v);
            }
            // One commit per drag / typed value, not one per frame.
            if (ImGui::IsItemDeactivatedAfterEdit()) note(Change::structure);
            if (ImGui::IsItemHovered()) {
                tooltip_ = std::string(p.help) + "\n" + range_hint(p);
            }
        }
    }
    // Parameters the leaf does not declare (typos in hand-written JSON).
    for (std::size_t i = 0; i < node.params.size(); ++i) {
        BtGraphParam& p = node.params[i];
        if (info != nullptr && info->find_param(p.name) != nullptr) continue;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x")) {
            node.params.erase(node.params.begin() + static_cast<std::ptrdiff_t>(i));
            note(Change::structure);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "%s = %g", p.name.c_str(), static_cast<double>(p.value));
        if (ImGui::IsItemHovered()) tooltip_ = "Not a parameter of this leaf: remove it.";
        ImGui::PopID();
    }
    ImGui::PopItemWidth();
}

void BehaviorPanel::handle_create() {
    if (ned::BeginCreate(ImVec4(1.0F, 1.0F, 1.0F, 1.0F), 2.0F)) {
        ned::PinId a;
        ned::PinId b;
        if (ned::QueryNewLink(&a, &b) && a && b) {
            const std::uintptr_t ta = widget_tag(a.Get());
            const std::uintptr_t tb = widget_tag(b.Get());
            if (ta == tb) {
                ned::RejectNewItem(kErrorColor, 2.0F);
                tooltip_ = "Link a parent's bottom pin to a child's top pin.";
            } else {
                const std::uint32_t parent = graph_id(ta == kTagOut ? a.Get() : b.Get());
                const std::uint32_t child = graph_id(ta == kTagOut ? b.Get() : a.Get());
                if (auto ok = bt_graph_can_link(graph_, parent, child); !ok) {
                    ned::RejectNewItem(kErrorColor, 2.0F);
                    tooltip_ = ok.error().message;
                } else {
                    if (bt_graph_parent(graph_, child) != 0) {
                        tooltip_ = "Moves the node under this parent.";
                    }
                    if (ned::AcceptNewItem(kRunningColor, 3.0F)) {
                        (void)bt_graph_link(graph_, parent, child);
                        note(Change::structure);
                    }
                }
            }
        }
        ned::PinId from;
        if (ned::QueryNewNode(&from) && from && ned::AcceptNewItem()) {
            // Dropped on empty canvas: create a node there, linked to `from`.
            const ImVec2 mouse = ImGui::GetMousePos(); // canvas space here
            create_from_pin_ = from.Get();
            create_pos_ = {mouse.x, mouse.y};
            ned::Suspend();
            ImGui::OpenPopup("bt_create");
            ned::Resume();
        }
    }
    ned::EndCreate();
}

void BehaviorPanel::handle_delete() {
    if (ned::BeginDelete()) {
        ned::LinkId link;
        while (ned::QueryDeletedLink(&link)) {
            if (ned::AcceptDeletedItem()) {
                const std::uint32_t child = graph_id(link.Get());
                if (bt_graph_unlink(graph_, bt_graph_parent(graph_, child), child)) {
                    note(Change::structure);
                }
            }
        }
        ned::NodeId node;
        while (ned::QueryDeletedNode(&node)) {
            if (ned::AcceptDeletedItem()) {
                bt_graph_remove_node(graph_, graph_id(node.Get()));
                note(Change::structure);
            }
        }
    }
    ned::EndDelete();
}

void BehaviorPanel::draw_context_menus() {
    const ImVec2 mouse = ImGui::GetMousePos(); // canvas space while the editor is active
    ned::Suspend();
    ned::NodeId node;
    ned::LinkId link;
    if (ned::ShowNodeContextMenu(&node)) {
        context_node_ = graph_id(node.Get());
        ImGui::OpenPopup("bt_node_menu");
    } else if (ned::ShowLinkContextMenu(&link)) {
        context_link_child_ = graph_id(link.Get());
        ImGui::OpenPopup("bt_link_menu");
    } else if (ned::ShowBackgroundContextMenu()) {
        create_from_pin_ = 0;
        create_pos_ = {mouse.x, mouse.y};
        ImGui::OpenPopup("bt_create");
    }

    if (ImGui::BeginPopup("bt_node_menu")) {
        if (BtGraphNode* n = graph_.find(context_node_)) {
            const BtGraphNode* parent = graph_.find(bt_graph_parent(graph_, n->id));
            const bool first = parent != nullptr && parent->children.front() == n->id;
            const bool last = parent != nullptr && parent->children.back() == n->id;
            ImGui::TextDisabled("%s", n->kind == BtKind::leaf ? n->leaf.c_str()
                                                              : display_name(n->kind));
            ImGui::Separator();
            if (ImGui::MenuItem("Move left", nullptr, false, parent != nullptr && !first)) {
                bt_graph_move_child(graph_, n->id, -1);
                push_all_positions_ = true;
                note(Change::structure);
            }
            if (ImGui::MenuItem("Move right", nullptr, false, parent != nullptr && !last)) {
                bt_graph_move_child(graph_, n->id, +1);
                push_all_positions_ = true;
                note(Change::structure);
            }
            if (n->kind != BtKind::leaf && ImGui::BeginMenu("Change to")) {
                for (const BtKind k : kComposites) {
                    const bool allowed = k != n->kind &&
                                         (k != BtKind::inverter || n->children.size() <= 1);
                    if (ImGui::MenuItem(display_name(k), nullptr, false, allowed)) {
                        n->kind = k;
                        note(Change::structure);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Detach from parent", nullptr, false, parent != nullptr)) {
                bt_graph_unlink(graph_, parent->id, n->id);
                note(Change::structure);
            }
            if (ImGui::MenuItem("Delete", "Del")) ned::DeleteNode(node_id(n->id));
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("bt_link_menu")) {
        if (ImGui::MenuItem("Delete link", "Del")) ned::DeleteLink(link_id(context_link_child_));
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("bt_create")) {
        const std::uint32_t from = graph_id(create_from_pin_);
        const std::uintptr_t from_tag = widget_tag(create_from_pin_);
        // From a top (input) pin the new node becomes the parent: composites only.
        const bool leaves_allowed = create_from_pin_ == 0 || from_tag == kTagOut;
        std::uint32_t created = 0;
        ImGui::TextDisabled("Composites");
        for (const BtKind k : kComposites) {
            if (ImGui::MenuItem(display_name(k))) {
                created = bt_graph_add_node(graph_, k, "", create_pos_);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kind_help(k));
        }
        ImGui::Separator();
        ImGui::TextDisabled("Leaves");
        for (const scene::BtLeafInfo& info : scene::bt_builtin_leaf_info()) {
            const std::string name(info.name);
            if (ImGui::MenuItem(name.c_str(), nullptr, false, leaves_allowed)) {
                created = bt_graph_add_node(graph_, BtKind::leaf, info.name, create_pos_);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.*s", static_cast<int>(info.help.size()),
                                                          info.help.data());
        }
        if (created != 0) {
            push_positions_.push_back(created);
            if (create_from_pin_ != 0 && graph_.find(from) != nullptr) {
                if (from_tag == kTagOut) {
                    (void)bt_graph_link(graph_, from, created);
                } else {
                    (void)bt_graph_link(graph_, created, from);
                }
            }
            create_from_pin_ = 0;
            note(Change::structure);
        }
        ImGui::EndPopup();
    }
    ned::Resume();
}

void BehaviorPanel::read_back_positions() {
    bool moved = false;
    for (BtGraphNode& n : graph_.nodes) {
        const ImVec2 p = ned::GetNodePosition(node_id(n.id));
        if (p.x == FLT_MAX || p.y == FLT_MAX) continue; // not created yet
        // ned floors positions; ignore sub-pixel differences so merely viewing
        // a tree never rewrites its source.
        if (std::abs(p.x - n.pos.x) >= 1.0F || std::abs(p.y - n.pos.y) >= 1.0F) {
            n.pos = {p.x, p.y};
            moved = true;
        }
    }
    dragging_layout_ = dragging_layout_ || moved;
    if (dragging_layout_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_layout_ = false;
        note(Change::layout); // commit() re-sorts children by x
    }
}

} // namespace engine::editor
