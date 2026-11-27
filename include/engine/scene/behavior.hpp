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
//
// The root object may also carry an "editor" field (the visual editor's node
// ids and layout, see src/editor/bt_graph.hpp); the runtime ignores it.

#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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

// Root-level field the runtime ignores (editor metadata: node ids, layout).
inline constexpr std::string_view bt_editor_field = "editor";
// Deepest nesting a tree may have (the root is depth 0).
inline constexpr int bt_max_depth = 64;

// ---------------------------------------------------------------------------
// Leaf parameter reflection for tools (the visual editor): what each built-in
// leaf reads from its JSON node, with defaults and accepted ranges. The
// built-in factories bind their parameters through this table, so it is the
// single source of truth and tools never hardcode leaf parameters.
// ---------------------------------------------------------------------------

// JSON type a parameter accepts (the built-in leaves only take numbers).
enum class BtParamType : std::uint8_t { number };

struct BtParamInfo {
    std::string_view name;
    BtParamType      type = BtParamType::number;
    float            default_value = 0.0F; // used when the field is absent
    float            min = std::numeric_limits<float>::lowest();
    float            max = std::numeric_limits<float>::max();
    // Absence means something other than the default (move_to without `y`
    // snaps its target to the ground), so tools must be able to omit it.
    bool             optional = false;
    std::string_view help;
};

struct BtLeafInfo {
    std::string_view             name;
    std::string_view             help;
    std::span<const BtParamInfo> params;

    [[nodiscard]] const BtParamInfo* find_param(std::string_view param) const {
        for (const BtParamInfo& p : params) {
            if (p.name == param) return &p;
        }
        return nullptr;
    }
};

// The built-in leaves (move_to, wait, near_point, near_camera) in a stable
// display order.
[[nodiscard]] std::span<const BtLeafInfo> bt_builtin_leaf_info();
// The built-in leaf called `name`, or null.
[[nodiscard]] const BtLeafInfo* bt_find_leaf_info(std::string_view name);

class BehaviorTreeAsset {
public:
    // Parses {"type": <composite>|"leaf", "name":..., "children":[...], ...}.
    // Nodes are stored depth-first pre-order (root, then each child's subtree
    // in order); tools map their nodes onto node_state by that order.
    // Unknown leaf names fail here, not at tick time. `extra_leaves` adds (or
    // overrides) leaf types beyond the built-ins (game code, tests).
    [[nodiscard]] static std::expected<std::shared_ptr<const BehaviorTreeAsset>, core::Error>
    parse(std::string_view json_text, const BtLeafFactories* extra_leaves = nullptr);

    enum class Kind : std::uint8_t {
        sequence, selector, reactive_sequence, reactive_selector, inverter, leaf
    };
    // The JSON "type" spelling of each kind ("leaf" for leaves), and back.
    [[nodiscard]] static std::string_view kind_name(Kind kind);
    [[nodiscard]] static std::optional<Kind> kind_from_name(std::string_view name);

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

// Node kinds for tools (the visual editor, the remote bt.leaves method): the
// JSON "type" spelling, how many children each takes, and what it does.
struct BtKindInfo {
    BehaviorTreeAsset::Kind kind = BehaviorTreeAsset::Kind::leaf;
    std::string_view        name;
    int                     min_children = 0;
    int                     max_children = 0; // -1: no limit
    std::string_view        help;
};

// Every kind: the composites in a stable display order, then "leaf".
[[nodiscard]] std::span<const BtKindInfo> bt_kind_info();
[[nodiscard]] const BtKindInfo& bt_find_kind_info(BehaviorTreeAsset::Kind kind);

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
