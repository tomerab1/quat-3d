#pragma once

// Behaviour trees as data assets (Phase 13, Slice 13.3).
//
// Trees are JSON; leaves are C++ callbacks registered by NAME and instantiated
// per node from the node's JSON parameters (a leaf factory), so designers
// compose behaviour without recompiling. Per-entity, per-node memory lives in
// the BehaviorTree component (BtNodeState), so one compiled asset serves many
// entities.
//
// Composites (standard semantics, as in BehaviorTree.CPP):
//   sequence           runs children in order; remembers the running child and
//                      resumes there next tick (finished steps are not redone).
//   selector           tries children in order until one does not fail;
//                      remembers the running child and resumes there.
//   reactive_sequence  re-checks every child from the first each tick: a
//                      condition that stops holding aborts the running child.
//   reactive_selector  re-evaluates higher-priority children each tick: one
//                      that succeeds or runs pre-empts the running lower one.
//   inverter           swaps success and failure of its single child.
// A node that ran last tick but is not reached this tick (pre-empted, or its
// parent finished) is aborted: running leaves are halted (move_to stops its
// NavAgent) and its memory resets, so it starts fresh when reached again. A
// composite that finishes resets its subtree; so does the root.
//
// Built-in leaves:
//   move_to    {x, y?, z, speed?}  drive the entity's NavAgent; running until it
//                                  arrives (success) or gives up (failure:
//                                  unreachable target, repeated failed plans,
//                                  no progress) or is stopped. Without `y` the
//                                  target snaps to the ground at (x, z) (the
//                                  navmesh surface, else BtWorld::ground_height).
//                                  While its parent keeps ticking it after
//                                  success it stays succeeded (no re-aim).
//   wait       {seconds}           running until the timer elapses, then success.
//   near_point {x, y, z, range}    condition: entity within range of the point.
//   near_camera{range}             condition: within range of the viewer (the
//                                  perception input; see BtWorld).

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/navigation.hpp"

namespace engine::core {
class JsonFields;
}

namespace engine::scene {

struct BehaviorTree;

enum class BtStatus : std::uint8_t { success, failure, running };

// What the trees perceive and act through, supplied by the caller per tick.
struct BtWorld {
    // The baked navmesh (move_to snaps y-less targets to its surface).
    const nav::NavMesh* navmesh = nullptr;
    // Authoritative ground height (fallback for y-less move_to targets).
    GroundHeightFn ground_height{};
    // Where the player / viewer is (near_camera). Unset: the active Camera
    // entity's position, if any.
    std::optional<glm::vec3> viewer{};
};

// Everything a leaf sees when it is ticked or halted.
struct BtLeafContext {
    entt::registry& registry;
    entt::entity    entity;
    const BtWorld&  world;
    float           dt;
    float&          state; // this node's per-entity scratch (BtNodeState::scratch)
};

// A leaf instance. `halt` (optional) runs when the leaf is aborted while
// running — undo whatever the tick started (e.g. stop the NavAgent).
struct BtLeaf {
    std::function<BtStatus(BtLeafContext&)> tick;
    std::function<void(BtLeafContext&)>     halt;
};

// Builds a leaf from its JSON node (parameters are read through `fields`,
// which rejects unread fields afterwards).
using BtLeafFactory = std::function<std::expected<BtLeaf, core::Error>(core::JsonFields&)>;
using BtLeafFactories = std::unordered_map<std::string, BtLeafFactory>;

class BehaviorTreeAsset {
public:
    // Parses {"type": <composite>|"leaf", "name":..., "children":[...], ...}.
    // Unknown leaf names fail here, not at tick time. `extra_leaves` adds (or
    // overrides) leaf types beyond the built-ins (game code, tests).
    [[nodiscard]] static std::expected<std::shared_ptr<const BehaviorTreeAsset>, core::Error>
    parse(std::string_view json_text, const BtLeafFactories* extra_leaves = nullptr);

    enum class Kind : std::uint8_t {
        sequence, selector, reactive_sequence, reactive_selector, inverter, leaf
    };
    struct Node {
        Kind                       kind = Kind::leaf;
        BtLeaf                     leaf;     // leaf nodes only
        std::vector<std::uint32_t> children; // composite nodes only
    };

    [[nodiscard]] const std::vector<Node>& nodes() const { return nodes_; }
    [[nodiscard]] std::uint32_t root() const { return 0; }

private:
    std::vector<Node> nodes_;
};

// Tick one tree once: `bt.compiled` must be set (node_state is sized to it on
// first use). Returns the root's status. Exposed for tests and tools;
// bt_system is the per-frame entry point.
BtStatus bt_tick(BehaviorTree& bt, entt::registry& registry, entt::entity entity,
                 const BtWorld& world, float dt);

// Tick every enabled BehaviorTree component: lazily compile its JSON (a parse
// error logs once and disables the tree), then evaluate it. Call before
// nav_agent_system so move_to decisions land the same frame.
void bt_system(entt::registry& registry, float dt, const BtWorld& world = {});

} // namespace engine::scene
