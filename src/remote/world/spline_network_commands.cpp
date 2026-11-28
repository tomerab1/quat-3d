// spline.network (ROADMAP G18.2): the curve network of a group of splines.
// See docs/remote-control.md and docs/splines.md.

#include <cmath>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/scene/scene.hpp"
#include "engine/scene/spline.hpp"
#include "internal.hpp"
#include "spline_internal.hpp"

namespace engine::remote::world {

namespace {

constexpr float k_pi = 3.14159265358979F;

RpcResult spline_network(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::uint32_t network = 0;
    spline::NetworkOptions o;
    float straight_deg = 15.0F;
    if (!f.has("network")) f.fail("network", "required: the splines' network number (> 0)");
    f.integer("network", network, 1, 0xFFFFFFFFLL)
        .number("snap_radius", o.snap_radius, 1.0e-4F, 100.0F)
        .number("straight_deg", straight_deg, 0.0F, 90.0F);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    o.straight_cos = std::cos(straight_deg * (k_pi / 180.0F));
    auto net = scene::build_curve_network(ctx.scene->registry(), network, o);
    if (!net) {
        const bool missing = net.error().message.find("no spline has it") != std::string::npos;
        return rpc_fail(missing ? rpc_code::not_found : rpc_code::invalid_params, net.error().message);
    }
    Json nodes = Json::array();
    for (const spline::NetworkNode& n : net->network.nodes) {
        Json ends = Json::array();
        for (const spline::NodeEnd& e : n.ends) {
            ends.push_back({{"edge", id_string(net->network.edges[e.edge].id)},
                            {"at_start", e.at_start},
                            {"direction", Json::array({e.direction.x, e.direction.y})}});
        }
        nodes.push_back({{"id", id_string(n.id)},
                         {"kind", std::string(spline::node_kind_name(n.kind))},
                         {"position", vec3_json(n.position)},
                         {"ends", std::move(ends)}});
    }
    Json edges = Json::array();
    for (const spline::NetworkEdge& e : net->network.edges) {
        edges.push_back({{"id", id_string(e.id)},
                         {"entity", entity_json(net->entities[e.curve_index])},
                         {"spline_id", e.curve_id},
                         {"t0", e.t0},
                         {"t1", e.t1},
                         {"length", e.length},
                         {"from", id_string(net->network.nodes[e.node_a].id)},
                         {"to", id_string(net->network.nodes[e.node_b].id)}});
    }
    return Json{{"network", network}, {"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
}

} // namespace

std::string id_string(std::uint64_t id) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

void register_spline_network_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("spline.network",
                      "The curve network of the splines whose `network` is N: {network, snap_radius?=0.25, "
                      "straight_deg?=15} -> {nodes: [{id, kind (end | straight | bend | tee | cross | n_way), "
                      "position, ends}], edges: [{id, entity, spline_id, t0, t1, length, from, to}]}.",
                      [ctx](const Json& p) { return spline_network(*ctx, p); });
}

} // namespace engine::remote::world
