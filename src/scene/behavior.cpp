#include "engine/scene/behavior.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;
using Kind = BehaviorTreeAsset::Kind;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] glm::vec3 entity_pos(const entt::registry& r, entt::entity e) {
    const auto* t = r.try_get<Transform>(e);
    return t != nullptr ? glm::vec3(t->world[3]) : glm::vec3(0.0F);
}

[[nodiscard]] float xz_distance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a.x, a.z) - glm::vec2(b.x, b.z));
}

// ---------------------------------------------------------------------------
// Built-in leaves. Parameters bind at parse time through checked reads (tree
// JSON is user-authored, and the engine has no exceptions — a mistyped field
// must fail the asset, never abort), so a bad leaf name, a wrong type or an
// out-of-range value fails the asset, not a tick. The caller rejects fields a
// leaf did not read (typo detection).
// ---------------------------------------------------------------------------

// move_to phases (BtNodeState::scratch).
constexpr float kMoveIdle = 0.0F;
constexpr float kMoveRunning = 1.0F;
constexpr float kMoveDone = 2.0F;

// The agent still heads where this leaf sent it (another system — a click, a
// different leaf — may have retargeted it since).
[[nodiscard]] bool aimed_at(const NavAgent& agent, const glm::vec3& target) {
    return xz_distance(agent.target, target) < 1e-3F;
}

std::expected<BtLeaf, core::Error> make_move_to(core::JsonFields& f) {
    glm::vec3 target(0.0F);
    float speed = 3.0F;
    f.number("x", target.x).number("y", target.y).number("z", target.z);
    f.number("speed", speed, 0.0F, 1000.0F);
    BtLeaf leaf;
    leaf.tick = [target, speed](BtLeafContext& c) {
        auto* agent = c.registry.try_get<NavAgent>(c.entity);
        if (agent == nullptr) return BtStatus::failure;
        if (c.state == kMoveDone) return BtStatus::success; // held: never re-aim
        if (c.state == kMoveIdle) {
            agent->target = target;
            agent->speed = speed;
            agent->active = true;
            agent->arrived = false;
            agent->path.clear();
            c.state = kMoveRunning;
            return BtStatus::running;
        }
        if (!aimed_at(*agent, target)) return BtStatus::failure; // overridden
        if (agent->arrived) {
            c.state = kMoveDone;
            return BtStatus::success;
        }
        if (!agent->active) return BtStatus::failure; // stopped short
        return BtStatus::running;
    };
    leaf.halt = [target](BtLeafContext& c) {
        // Pre-empted: stop the walk this leaf started, unless the agent has
        // been sent somewhere else meanwhile.
        auto* agent = c.registry.try_get<NavAgent>(c.entity);
        if (agent == nullptr || !agent->active || !aimed_at(*agent, target)) return;
        agent->active = false;
        agent->arrived = false;
        agent->path.clear();
    };
    return leaf;
}

std::expected<BtLeaf, core::Error> make_wait(core::JsonFields& f) {
    float seconds = 1.0F;
    f.number("seconds", seconds, 0.0F, 1.0e6F);
    BtLeaf leaf;
    leaf.tick = [seconds](BtLeafContext& c) {
        if (c.state >= seconds) return BtStatus::success; // held until reset
        c.state += c.dt;
        return c.state >= seconds ? BtStatus::success : BtStatus::running;
    };
    return leaf;
}

std::expected<BtLeaf, core::Error> make_near_point(core::JsonFields& f) {
    glm::vec3 point(0.0F);
    float range = 2.0F;
    f.number("x", point.x).number("y", point.y).number("z", point.z);
    f.number("range", range, 0.0F);
    BtLeaf leaf;
    leaf.tick = [point, range](BtLeafContext& c) {
        return xz_distance(entity_pos(c.registry, c.entity), point) <= range
                   ? BtStatus::success
                   : BtStatus::failure;
    };
    return leaf;
}

std::expected<BtLeaf, core::Error> make_near_camera(core::JsonFields& f) {
    float range = 5.0F;
    f.number("range", range, 0.0F);
    BtLeaf leaf;
    leaf.tick = [range](BtLeafContext& c) {
        std::optional<glm::vec3> viewer = c.world.viewer;
        if (!viewer) {
            const entt::entity cam = find_active_camera(c.registry);
            if (cam == entt::null) return BtStatus::failure; // nothing to perceive
            viewer = entity_pos(c.registry, cam);
        }
        return xz_distance(entity_pos(c.registry, c.entity), *viewer) <= range
                   ? BtStatus::success
                   : BtStatus::failure;
    };
    return leaf;
}

[[nodiscard]] const BtLeafFactories& builtin_leaves() {
    static const BtLeafFactories factories{
        {"move_to", &make_move_to},
        {"wait", &make_wait},
        {"near_point", &make_near_point},
        {"near_camera", &make_near_camera},
    };
    return factories;
}

[[nodiscard]] std::optional<Kind> composite_kind(const std::string& type) {
    if (type == "sequence") return Kind::sequence;
    if (type == "selector") return Kind::selector;
    if (type == "reactive_sequence") return Kind::reactive_sequence;
    if (type == "reactive_selector") return Kind::reactive_selector;
    if (type == "inverter") return Kind::inverter;
    return std::nullopt;
}

} // namespace

std::expected<std::shared_ptr<const BehaviorTreeAsset>, core::Error>
BehaviorTreeAsset::parse(std::string_view json_text, const BtLeafFactories* extra_leaves) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return fail("behavior tree: invalid JSON");

    const auto find_factory = [&](const std::string& name) -> const BtLeafFactory* {
        if (extra_leaves != nullptr) {
            if (const auto it = extra_leaves->find(name); it != extra_leaves->end()) {
                return &it->second;
            }
        }
        const auto it = builtin_leaves().find(name);
        return it != builtin_leaves().end() ? &it->second : nullptr;
    };

    auto asset = std::make_shared<BehaviorTreeAsset>();
    // Depth-first flatten; returns the node's index or an error. Depth is
    // bounded so hostile nesting cannot overflow the stack.
    constexpr int max_depth = 64;
    const std::function<std::expected<std::uint32_t, core::Error>(const json&, int,
                                                                  const std::string&)>
        build = [&](const json& n, int depth,
                    const std::string& where) -> std::expected<std::uint32_t, core::Error> {
        if (depth > max_depth) {
            return fail("behavior tree: nesting deeper than " + std::to_string(max_depth));
        }
        core::JsonFields f(n, where);
        std::string type;
        f.string("type", type, 64);
        if (!f.ok()) return std::unexpected(f.finish().error());
        if (type.empty()) return fail("behavior tree: " + where + " has no \"type\"");
        const auto index = static_cast<std::uint32_t>(asset->nodes_.size());
        asset->nodes_.emplace_back();

        if (type == "leaf") {
            std::string name;
            f.string("name", name, 64);
            const BtLeafFactory* factory = find_factory(name);
            if (factory == nullptr) {
                std::string known;
                for (const auto& [k, v] : builtin_leaves()) known += (known.empty() ? "" : ", ") + k;
                return fail("behavior tree: " + where + ": unknown leaf '" + name +
                            "' (known: " + known + ")");
            }
            auto leaf = (*factory)(f);
            if (!leaf) return std::unexpected(leaf.error());
            if (auto ok = f.finish(); !ok) return fail("behavior tree: " + ok.error().message);
            if (!leaf->tick) return fail("behavior tree: " + where + ": leaf has no tick");
            asset->nodes_[index].kind = Kind::leaf;
            asset->nodes_[index].leaf = std::move(*leaf);
            return index;
        }

        const std::optional<Kind> kind = composite_kind(type);
        if (!kind) {
            return fail("behavior tree: " + where + ": unknown node type '" + type +
                        "' (known: leaf, sequence, selector, reactive_sequence, "
                        "reactive_selector, inverter)");
        }
        asset->nodes_[index].kind = *kind;

        const json* children = f.raw("children");
        if (auto ok = f.finish(); !ok) return fail("behavior tree: " + ok.error().message);
        if (children == nullptr || !children->is_array() || children->empty()) {
            return fail("behavior tree: " + where + ": '" + type + "' needs a children array");
        }
        if (*kind == Kind::inverter && children->size() != 1) {
            return fail("behavior tree: " + where + ": inverter takes exactly one child");
        }
        for (std::size_t i = 0; i < children->size(); ++i) {
            auto child = build((*children)[i], depth + 1,
                               where + ".children[" + std::to_string(i) + "]");
            if (!child) return std::unexpected(child.error());
            asset->nodes_[index].children.push_back(*child);
        }
        return index;
    };
    if (auto r = build(root, 0, "root"); !r) return std::unexpected(r.error());
    return std::shared_ptr<const BehaviorTreeAsset>(std::move(asset));
}

namespace {

// One evaluation of one entity's tree. Nodes record the tick stamp when
// reached; after the pass, nodes that ran last tick but were not reached are
// aborted (pre-emption).
struct Ticker {
    const BehaviorTreeAsset&  tree;
    std::vector<BtNodeState>& st;
    entt::registry&           registry;
    entt::entity              entity;
    const BtWorld&            world;
    float                     dt;
    std::uint32_t             stamp;

    void halt_and_reset(std::uint32_t i) {
        BtNodeState& ns = st[i];
        const BehaviorTreeAsset::Node& node = tree.nodes()[i];
        if (ns.running && node.kind == Kind::leaf && node.leaf.halt) {
            BtLeafContext c{registry, entity, world, dt, ns.scratch};
            node.leaf.halt(c);
        }
        ns = BtNodeState{};
    }

    void reset_subtree(std::uint32_t i) {
        for (const std::uint32_t c : tree.nodes()[i].children) {
            halt_and_reset(c);
            reset_subtree(c);
        }
    }

    BtStatus tick(std::uint32_t i) {
        const BehaviorTreeAsset::Node& node = tree.nodes()[i];
        st[i].ticked = stamp;
        BtStatus s = BtStatus::failure;
        switch (node.kind) {
        case Kind::leaf: {
            BtLeafContext c{registry, entity, world, dt, st[i].scratch};
            s = node.leaf.tick(c);
            break;
        }
        case Kind::sequence:
        case Kind::selector: {
            // Memory composites: resume at the child that was running.
            const BtStatus stop_on =
                node.kind == Kind::sequence ? BtStatus::failure : BtStatus::success;
            const BtStatus all_done =
                node.kind == Kind::sequence ? BtStatus::success : BtStatus::failure;
            s = all_done;
            for (std::uint32_t k = st[i].child; k < node.children.size(); ++k) {
                const BtStatus cs = tick(node.children[k]);
                if (cs == BtStatus::running) {
                    st[i].child = k;
                    s = BtStatus::running;
                    break;
                }
                if (cs == stop_on) {
                    s = stop_on;
                    break;
                }
            }
            break;
        }
        case Kind::reactive_sequence:
        case Kind::reactive_selector: {
            // Re-evaluated from the first child every tick; whichever child
            // was running before and is not reached now gets aborted.
            const BtStatus stop_on =
                node.kind == Kind::reactive_sequence ? BtStatus::failure : BtStatus::success;
            s = node.kind == Kind::reactive_sequence ? BtStatus::success : BtStatus::failure;
            for (std::uint32_t k = 0; k < node.children.size(); ++k) {
                const BtStatus cs = tick(node.children[k]);
                if (cs == BtStatus::running || cs == stop_on) {
                    st[i].child = k;
                    s = cs;
                    break;
                }
            }
            break;
        }
        case Kind::inverter: {
            s = tick(node.children[0]);
            if (s == BtStatus::success) s = BtStatus::failure;
            else if (s == BtStatus::failure) s = BtStatus::success;
            break;
        }
        }
        if (s != BtStatus::running && node.kind != Kind::leaf) {
            // A finished composite starts over next time: reset its subtree
            // (halting anything a reactive branch left running).
            reset_subtree(i);
            st[i].child = 0;
        }
        st[i].running = s == BtStatus::running;
        return s;
    }
};

} // namespace

BtStatus bt_tick(BehaviorTree& bt, entt::registry& registry, entt::entity entity,
                 const BtWorld& world, float dt) {
    if (bt.compiled == nullptr || bt.compiled->nodes().empty()) return BtStatus::failure;
    const BehaviorTreeAsset& tree = *bt.compiled;
    if (bt.node_state.size() != tree.nodes().size()) {
        bt.node_state.assign(tree.nodes().size(), BtNodeState{});
    }
    // Stamps start at 1 (0 = never ticked); on wrap-around everything resets.
    if (++bt.tick_count == 0) {
        bt.node_state.assign(tree.nodes().size(), BtNodeState{});
        bt.tick_count = 1;
    }
    Ticker t{tree, bt.node_state, registry, entity, world, dt, bt.tick_count};
    const BtStatus status = t.tick(tree.root());

    // Pre-emption: whatever was not reached this tick is aborted.
    for (std::uint32_t i = 0; i < tree.nodes().size(); ++i) {
        if (bt.node_state[i].ticked != bt.tick_count) t.halt_and_reset(i);
    }
    // A finished tree starts over next tick.
    if (status != BtStatus::running) {
        bt.node_state.assign(tree.nodes().size(), BtNodeState{});
    }
    return status;
}

void bt_system(entt::registry& registry, float dt, const BtWorld& world) {
    for (auto [e, bt] : registry.view<BehaviorTree>().each()) {
        if (!bt.enabled || bt.source.empty()) continue;
        if (bt.compiled == nullptr) {
            if (bt.parse_failed) continue;
            auto parsed = BehaviorTreeAsset::parse(bt.source);
            if (!parsed) {
                bt.parse_failed = true;
                std::fprintf(stderr, "[bt] parse failed: %s\n", parsed.error().message.c_str());
                continue;
            }
            bt.compiled = std::move(*parsed);
            bt.node_state.assign(bt.compiled->nodes().size(), BtNodeState{});
        }
        (void)bt_tick(bt, registry, e, world, dt);
    }
}

} // namespace engine::scene
