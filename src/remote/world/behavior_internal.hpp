#pragma once

// Private helpers shared by the bt.* command files (behavior_commands.cpp,
// behavior_edit_commands.cpp). Contract: docs/remote-control.md,
// "Behaviour-tree authoring".

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "engine/scene/bt_graph.hpp"
#include "internal.hpp"

namespace engine::scene {
struct BehaviorTree;
}

namespace engine::remote::world::bt {

// The entity's BehaviorTree, or not_found saying how to create one.
[[nodiscard]] std::expected<scene::BehaviorTree*, RpcError> tree_of(entt::registry& registry,
                                                                    entt::entity entity);

// The stored tree as a graph (empty when there is no tree), or `failed` when
// the stored JSON cannot be shown as a graph.
[[nodiscard]] std::expected<scene::BtGraph, RpcError> load_graph(const scene::BehaviorTree& bt);

// The <tree view> of the entity's tree.
[[nodiscard]] Json tree_view(const entt::registry& registry, entt::entity entity);

// Validates `graph` (per-node issues as invalid_params with data.issues),
// serialises it and compiles it with the runtime. Returns the source text.
[[nodiscard]] std::expected<std::string, RpcError> compile(const scene::BtGraph& graph);

// Stores compiled source text and has the tree recompiled on its next tick,
// as the inspector and the Behaviour panel do.
void write_source(scene::BehaviorTree& bt, std::string text);

// A node id parameter (`field` names it) that must exist in `graph`.
[[nodiscard]] std::expected<std::uint32_t, RpcError>
read_node(const Json* value, const scene::BtGraph& graph, std::string_view field);

// A tree-JSON error from the graph reader, without its "behavior tree: "
// prefix and with the leading "root" replaced by `where`.
[[nodiscard]] std::string rebase_error(std::string message, std::string_view where);

// "a, b, c" of every node kind / built-in leaf name.
[[nodiscard]] std::string known_kinds();
[[nodiscard]] std::string known_leaves();

} // namespace engine::remote::world::bt
