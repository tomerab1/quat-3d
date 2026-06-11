#include "engine/scene/behavior.hpp"

#include <cstdio>
#include <unordered_map>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/nav/navmesh.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] glm::vec3 entity_pos(entt::registry& r, entt::entity e) {
    const auto* t = r.try_get<Transform>(e);
    return t != nullptr ? glm::vec3(t->world[3]) : glm::vec3(0.0F);
}

[[nodiscard]] float xz_distance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a.x, a.z) - glm::vec2(b.x, b.z));
}

// Leaf factories: name -> (node json -> leaf instance). Parameters bind at
// parse time, so a bad leaf name or missing field fails the asset, not a tick.
using LeafFactory = std::expected<BtLeafFn, core::Error> (*)(const json&);

std::expected<BtLeafFn, core::Error> make_move_to(const json& n) {
    const glm::vec3 target(n.value("x", 0.0F), n.value("y", 0.0F), n.value("z", 0.0F));
    const float speed = n.value("speed", 3.0F);
    return BtLeafFn([target, speed](entt::registry& r, entt::entity e, float, float& state) {
        auto* agent = r.try_get<NavAgent>(e);
        if (agent == nullptr) return BtStatus::failure;
        if (state < 0.5F) { // not started: aim the agent
            agent->target = target;
            agent->speed = speed;
            agent->active = true;
            agent->arrived = false;
            agent->path.clear();
            state = 1.0F;
            return BtStatus::running;
        }
        if (agent->arrived) {
            state = 0.0F;
            return BtStatus::success;
        }
        if (!agent->active) { // the nav system gave up (no route)
            state = 0.0F;
            return BtStatus::failure;
        }
        return BtStatus::running;
    });
}

std::expected<BtLeafFn, core::Error> make_wait(const json& n) {
    const float seconds = n.value("seconds", 1.0F);
    return BtLeafFn([seconds](entt::registry&, entt::entity, float dt, float& state) {
        state += dt;
        if (state >= seconds) {
            state = 0.0F;
            return BtStatus::success;
        }
        return BtStatus::running;
    });
}

std::expected<BtLeafFn, core::Error> make_near_point(const json& n) {
    const glm::vec3 point(n.value("x", 0.0F), n.value("y", 0.0F), n.value("z", 0.0F));
    const float range = n.value("range", 2.0F);
    return BtLeafFn([point, range](entt::registry& r, entt::entity e, float, float&) {
        return xz_distance(entity_pos(r, e), point) <= range ? BtStatus::success
                                                             : BtStatus::failure;
    });
}

std::expected<BtLeafFn, core::Error> make_near_camera(const json& n) {
    const float range = n.value("range", 5.0F);
    return BtLeafFn([range](entt::registry& r, entt::entity e, float, float&) {
        for (auto [ce, cam, ct] : r.view<const Camera, const Transform>().each()) {
            if (!cam.is_active) continue;
            return xz_distance(entity_pos(r, e), glm::vec3(ct.world[3])) <= range
                       ? BtStatus::success
                       : BtStatus::failure;
        }
        return BtStatus::failure; // no perceivable camera
    });
}

[[nodiscard]] const std::unordered_map<std::string, LeafFactory>& leaf_factories() {
    static const std::unordered_map<std::string, LeafFactory> factories{
        {"move_to", &make_move_to},
        {"wait", &make_wait},
        {"near_point", &make_near_point},
        {"near_camera", &make_near_camera},
    };
    return factories;
}

} // namespace

std::expected<std::shared_ptr<const BehaviorTreeAsset>, core::Error>
BehaviorTreeAsset::parse(std::string_view json_text) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return fail("behavior tree: invalid JSON");

    auto asset = std::make_shared<BehaviorTreeAsset>();
    // Depth-first flatten; returns the node's index or an error.
    const std::function<std::expected<std::uint32_t, core::Error>(const json&)> build =
        [&](const json& n) -> std::expected<std::uint32_t, core::Error> {
        if (!n.is_object() || !n.contains("type")) {
            return fail("behavior tree: node without a type");
        }
        const std::string type = n["type"].get<std::string>();
        const auto index = static_cast<std::uint32_t>(asset->nodes_.size());
        asset->nodes_.emplace_back();

        if (type == "leaf") {
            const std::string name = n.value("name", "");
            const auto it = leaf_factories().find(name);
            if (it == leaf_factories().end()) {
                return fail("behavior tree: unknown leaf '" + name + "'");
            }
            auto leaf = it->second(n);
            if (!leaf) return std::unexpected(leaf.error());
            asset->nodes_[index].kind = Kind::leaf;
            asset->nodes_[index].leaf = std::move(*leaf);
            return index;
        }

        Kind kind{};
        if (type == "sequence") kind = Kind::sequence;
        else if (type == "selector") kind = Kind::selector;
        else if (type == "inverter") kind = Kind::inverter;
        else return fail("behavior tree: unknown node type '" + type + "'");
        asset->nodes_[index].kind = kind;

        if (!n.contains("children") || !n["children"].is_array() || n["children"].empty()) {
            return fail("behavior tree: composite '" + type + "' needs children");
        }
        if (kind == Kind::inverter && n["children"].size() != 1) {
            return fail("behavior tree: inverter takes exactly one child");
        }
        for (const json& c : n["children"]) {
            auto child = build(c);
            if (!child) return std::unexpected(child.error());
            asset->nodes_[index].children.push_back(*child);
        }
        return index;
    };
    if (auto r = build(root); !r) return std::unexpected(r.error());
    return std::shared_ptr<const BehaviorTreeAsset>(std::move(asset));
}

namespace {

BtStatus tick_node(const BehaviorTreeAsset& tree, std::uint32_t index, entt::registry& r,
                   entt::entity e, float dt, std::vector<float>& state) {
    const BehaviorTreeAsset::Node& node = tree.nodes()[index];
    switch (node.kind) {
    case BehaviorTreeAsset::Kind::leaf:
        return node.leaf(r, e, dt, state[index]);
    case BehaviorTreeAsset::Kind::sequence:
        for (const std::uint32_t c : node.children) {
            const BtStatus s = tick_node(tree, c, r, e, dt, state);
            if (s != BtStatus::success) return s;
        }
        return BtStatus::success;
    case BehaviorTreeAsset::Kind::selector:
        for (const std::uint32_t c : node.children) {
            const BtStatus s = tick_node(tree, c, r, e, dt, state);
            if (s != BtStatus::failure) return s;
        }
        return BtStatus::failure;
    case BehaviorTreeAsset::Kind::inverter: {
        const BtStatus s = tick_node(tree, node.children[0], r, e, dt, state);
        if (s == BtStatus::success) return BtStatus::failure;
        if (s == BtStatus::failure) return BtStatus::success;
        return s;
    }
    }
    return BtStatus::failure;
}

} // namespace

void bt_system(entt::registry& registry, float dt) {
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
            bt.node_state.assign(bt.compiled->nodes().size(), 0.0F);
        }
        tick_node(*bt.compiled, bt.compiled->root(), registry, e, dt, bt.node_state);
    }
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error> run_behavior_self_test() {
    // The walled plate from the nav tests; the tree patrols to the far side.
    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> idx;
    const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        verts.insert(verts.end(), {a, b, c, d});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad({-20.0F, 0.0F, -20.0F}, {-20.0F, 0.0F, 20.0F}, {20.0F, 0.0F, 20.0F},
         {20.0F, 0.0F, -20.0F});
    const float x0 = -20.0F, x1 = 12.0F, z0 = -0.5F, z1 = 0.5F, h = 3.0F;
    quad({x0, h, z0}, {x0, h, z1}, {x1, h, z1}, {x1, h, z0});
    quad({x0, 0.0F, z0}, {x0, h, z0}, {x1, h, z0}, {x1, 0.0F, z0});
    quad({x1, 0.0F, z1}, {x1, h, z1}, {x0, h, z1}, {x0, 0.0F, z1});
    quad({x1, 0.0F, z0}, {x1, h, z0}, {x1, h, z1}, {x1, 0.0F, z1});
    auto navmesh = nav::NavMesh::build(verts, idx);
    if (!navmesh) return std::unexpected(navmesh.error());

    entt::registry registry;
    const entt::entity e = registry.create();
    auto& t = registry.emplace<Transform>(e);
    t.local = glm::mat4(1.0F);
    t.local[3] = glm::vec4(-10.0F, 0.0F, -10.0F, 1.0F);
    t.world = t.local;
    registry.emplace<NavAgent>(e);
    auto& bt = registry.emplace<BehaviorTree>(e);
    bt.source = R"({"type":"selector","children":[
        {"type":"sequence","children":[
            {"type":"leaf","name":"near_point","x":-10,"z":10,"range":1.5},
            {"type":"leaf","name":"wait","seconds":0.5}]},
        {"type":"leaf","name":"move_to","x":-10,"z":10,"speed":4}]})";

    for (int i = 0; i < 60 * 60; ++i) {
        bt_system(registry, 1.0F / 60.0F);
        nav_agent_system(registry, *navmesh, 1.0F / 60.0F);
    }
    if (bt.parse_failed || bt.compiled == nullptr) {
        return std::unexpected(core::Error{"behavior self-test: tree failed to compile"});
    }
    const glm::vec3 final_pos(registry.get<Transform>(e).world[3]);
    if (xz_distance(final_pos, glm::vec3(-10.0F, 0.0F, 10.0F)) > 2.0F) {
        return std::unexpected(
            core::Error{"behavior self-test: move_to leaf did not reach the patrol point"});
    }
    // After arrival the selector's first branch runs: the wait timer must tick.
    bool wait_ticked = false;
    for (int i = 0; i < 20 && !wait_ticked; ++i) {
        bt_system(registry, 1.0F / 60.0F);
        // Node order (DFS): 0 selector, 1 sequence, 2 near_point, 3 wait, 4 move_to.
        wait_ticked = bt.node_state.size() > 3 && bt.node_state[3] > 0.0F;
    }
    if (!wait_ticked) {
        return std::unexpected(
            core::Error{"behavior self-test: wait leaf never ran after arrival"});
    }
    return {};
}

} // namespace engine::scene
