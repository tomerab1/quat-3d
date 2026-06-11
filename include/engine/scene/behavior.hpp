#pragma once

// Behaviour trees as data assets (Phase 13, Slice 13.3).
//
// Trees are JSON; leaves are C++ callbacks registered by NAME and instantiated
// per node from the node's JSON parameters (a leaf factory), so designers
// compose behaviour without recompiling. Composites: sequence, selector,
// inverter. Trees re-tick from the root every frame (stateless flow control);
// leaves needing memory (e.g. "wait") get one float of per-node, per-entity
// scratch.
//
// Built-in leaves:
//   move_to    {x, y, z, speed?}   drive the entity's NavAgent; running until
//                                  it arrives, failure if it cannot path.
//   wait       {seconds}           running until the timer elapses, then success.
//   near_point {x, y, z, range}    condition: entity within range of the point.
//   near_camera{range}             condition: within range of the active camera
//                                  (the perception stub).

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {

enum class BtStatus : std::uint8_t { success, failure, running };

// A leaf instance: ticked with the owning entity and its per-node scratch.
using BtLeafFn =
    std::function<BtStatus(entt::registry&, entt::entity, float dt, float& state)>;

class BehaviorTreeAsset {
public:
    // Parses {"type":"sequence"/"selector"/"inverter"/"leaf", "name":..., ...}.
    // Unknown leaf names fail here, not at tick time.
    [[nodiscard]] static std::expected<std::shared_ptr<const BehaviorTreeAsset>, core::Error>
    parse(std::string_view json_text);

    enum class Kind : std::uint8_t { sequence, selector, inverter, leaf };
    struct Node {
        Kind                       kind = Kind::leaf;
        BtLeafFn                   leaf;     // leaf nodes only
        std::vector<std::uint32_t> children; // composite nodes only
    };

    [[nodiscard]] const std::vector<Node>& nodes() const { return nodes_; }
    [[nodiscard]] std::uint32_t root() const { return 0; }

private:
    std::vector<Node> nodes_;
};

// Tick every enabled BehaviorTree component: lazily compile its JSON (a parse
// error logs once and disables the tree), then evaluate from the root. Call
// before nav_agent_system so move_to decisions land the same frame.
void bt_system(entt::registry& registry, float dt);

// Self-test: a patrol tree (selector: [near target -> wait] else move_to)
// drives a kinematic NavAgent across the walled plate and settles at the
// point; the wait timer then accumulates.
[[nodiscard]] std::expected<void, core::Error> run_behavior_self_test();

} // namespace engine::scene
