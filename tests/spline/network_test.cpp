// Curve networks (ROADMAP G18.2): snapping, T-junctions, crossings, node
// classification on fixed fixtures, and ids that survive local edits.

#include <algorithm>
#include <set>

#include <gtest/gtest.h>

#include "engine/spline/network.hpp"
#include "engine/spline/queries.hpp"

namespace quat::test {
namespace {

namespace sp = engine::spline;

[[nodiscard]] sp::Curve line(std::vector<glm::vec3> pts, sp::SplineKind kind = sp::SplineKind::polyline,
                             bool closed = false) {
    sp::Spline s;
    s.kind = kind;
    s.closed = closed;
    for (const glm::vec3& p : pts) s.points.push_back({p, 1.0F, 0.0F, 0});
    auto c = sp::Curve::build(s);
    EXPECT_TRUE(c.has_value());
    return c ? *c : sp::Curve{};
}

struct Net {
    std::vector<sp::Curve>        curves;
    std::vector<std::uint32_t>    ids;
    sp::CurveNetwork              net;

    void add(std::uint32_t id, sp::Curve c) {
        ids.push_back(id);
        curves.push_back(std::move(c));
    }
    void build(bool reversed = false) {
        std::vector<sp::NetworkCurve> in;
        for (std::size_t i = 0; i < curves.size(); ++i) in.push_back({ids[i], &curves[i]});
        if (reversed) std::reverse(in.begin(), in.end());
        auto r = sp::build_network(in);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        net = std::move(*r);
    }
    [[nodiscard]] std::size_t count(sp::NodeKind k) const {
        return static_cast<std::size_t>(
            std::count_if(net.nodes.begin(), net.nodes.end(), [k](const sp::NetworkNode& n) { return n.kind == k; }));
    }
    [[nodiscard]] const sp::NetworkNode& only(sp::NodeKind k) const {
        const auto it = std::find_if(net.nodes.begin(), net.nodes.end(),
                                     [k](const sp::NetworkNode& n) { return n.kind == k; });
        EXPECT_NE(it, net.nodes.end());
        return *it;
    }
    [[nodiscard]] std::set<std::uint64_t> node_ids() const {
        std::set<std::uint64_t> s;
        for (const auto& n : net.nodes) s.insert(n.id);
        return s;
    }
    [[nodiscard]] std::set<std::uint64_t> edge_ids() const {
        std::set<std::uint64_t> s;
        for (const auto& e : net.edges) s.insert(e.id);
        return s;
    }
};

TEST(SplineNetwork, CrossingSplitsBothCurvesIntoAnX) {
    Net n;
    n.add(1, line({{-5, 0, 0}, {5, 0, 0}}));
    n.add(2, line({{0, 0, -5}, {0, 0, 5}}));
    n.build();
    EXPECT_EQ(n.net.edges.size(), 4U);
    EXPECT_EQ(n.count(sp::NodeKind::end), 4U);
    ASSERT_EQ(n.count(sp::NodeKind::cross), 1U);
    const sp::NetworkNode& x = n.only(sp::NodeKind::cross);
    EXPECT_NEAR(glm::length(x.position), 0.0F, 1e-5F);
    // Ends counter-clockwise seen from above, starting at +X: +X, -Z, -X, +Z.
    ASSERT_EQ(x.ends.size(), 4U);
    EXPECT_NEAR(x.ends[0].direction.x, 1.0F, 1e-5F);
    EXPECT_NEAR(x.ends[1].direction.y, -1.0F, 1e-5F);
    EXPECT_NEAR(x.ends[2].direction.x, -1.0F, 1e-5F);
    EXPECT_NEAR(x.ends[3].direction.y, 1.0F, 1e-5F);
    for (const sp::NetworkEdge& e : n.net.edges) EXPECT_NEAR(e.length, 5.0F, 1e-4F);
}

TEST(SplineNetwork, EndpointOnAnInteriorMakesATee) {
    Net n;
    n.add(1, line({{-5, 0, 0}, {5, 0, 0}}));
    n.add(2, line({{1, 0, 0.2F}, {1, 0, 6}})); // starts 0.2 m off curve 1 (snap radius 0.25)
    n.build();
    ASSERT_EQ(n.count(sp::NodeKind::tee), 1U);
    EXPECT_EQ(n.count(sp::NodeKind::end), 3U);
    EXPECT_EQ(n.net.edges.size(), 3U);
    const sp::NetworkNode& t = n.only(sp::NodeKind::tee);
    EXPECT_NEAR(t.position.x, 1.0F, 1e-5F);
    EXPECT_NEAR(t.position.z, 0.1F, 1e-5F); // mean of the endpoint and its foot on curve 1
    // Curve 1 is split at x = 1: edges of 6 and 4 metres.
    std::vector<float> lengths;
    for (const auto& e : n.net.edges) {
        if (e.curve_id == 1) lengths.push_back(e.length);
    }
    ASSERT_EQ(lengths.size(), 2U);
    EXPECT_NEAR(lengths[0], 6.0F, 1e-4F);
    EXPECT_NEAR(lengths[1], 4.0F, 1e-4F);
}

TEST(SplineNetwork, SnappedEndpointsMakeBendsAndStraights) {
    Net n;
    n.add(1, line({{0, 0, 0}, {5, 0, 0}}));
    n.add(2, line({{5.1F, 0, 0}, {5, 0, 5}}));   // a corner
    n.add(3, line({{0, 0, 0.1F}, {-5, 0, 0.1F}})); // continues curve 1 straight on
    n.build();
    EXPECT_EQ(n.count(sp::NodeKind::bend), 1U);
    EXPECT_EQ(n.count(sp::NodeKind::straight), 1U);
    EXPECT_EQ(n.count(sp::NodeKind::end), 2U);
    EXPECT_NEAR(n.only(sp::NodeKind::bend).position.x, 5.05F, 1e-5F);
}

TEST(SplineNetwork, ManyWaysAndLoops) {
    Net n;
    // Five spokes meeting at the origin through their start points.
    const glm::vec3 tips[] = {{5, 0, 0}, {2, 0, 5}, {-4, 0, 3}, {-4, 0, -3}, {2, 0, -5}};
    for (std::uint32_t i = 0; i < 5; ++i) n.add(10 + i, line({{0, 0, 0}, tips[i]}));
    // A loop far away, touching nothing: one node, one edge around it.
    n.add(99, line({{50, 0, 0}, {60, 0, 0}, {60, 0, 10}, {50, 0, 10}}, sp::SplineKind::polyline, true));
    n.build();
    EXPECT_EQ(n.count(sp::NodeKind::n_way), 1U);
    EXPECT_EQ(n.only(sp::NodeKind::n_way).ends.size(), 5U);
    const auto loop_edge = std::find_if(n.net.edges.begin(), n.net.edges.end(),
                                        [](const sp::NetworkEdge& e) { return e.curve_id == 99; });
    ASSERT_NE(loop_edge, n.net.edges.end());
    EXPECT_EQ(loop_edge->node_a, loop_edge->node_b);
    EXPECT_NEAR(loop_edge->length, 40.0F, 1e-3F);
    EXPECT_EQ(n.net.nodes[loop_edge->node_a].kind, sp::NodeKind::bend); // the loop's corner at point 0
}

TEST(SplineNetwork, SelfCrossingSplitsTheCurveTwice) {
    Net n;
    n.add(7, line({{0, 0, 0}, {10, 0, 0}, {10, 0, 5}, {5, 0, 5}, {5, 0, -5}}));
    n.build();
    ASSERT_EQ(n.count(sp::NodeKind::cross), 1U);
    EXPECT_NEAR(n.only(sp::NodeKind::cross).position.x, 5.0F, 1e-4F);
    EXPECT_EQ(n.net.edges.size(), 3U); // start -> X, the loop X -> X, X -> end
}

TEST(SplineNetwork, IdsAreStableUnderLocalEditsAndInputOrder) {
    Net n;
    n.add(1, line({{-10, 0, 0}, {10, 0, 0}}));
    n.add(2, line({{0, 0, -5}, {0, 0, 5}}));
    n.add(3, line({{-10, 0, 0.1F}, {-10, 0, 8}})); // joins curve 1's start
    n.build();
    const auto nodes = n.node_ids();
    const auto edges = n.edge_ids();

    Net reversed = n;
    reversed.build(true);
    EXPECT_EQ(reversed.node_ids(), nodes);
    EXPECT_EQ(reversed.edge_ids(), edges);

    // Reshaping curve 2 without changing what it meets keeps every id.
    Net reshaped = n;
    reshaped.curves[1] = line({{0, 0, -5}, {1, 0, 0}, {0, 0, 5}}, sp::SplineKind::centripetal_catmull_rom);
    reshaped.build();
    EXPECT_EQ(reshaped.node_ids(), nodes);
    EXPECT_EQ(reshaped.edge_ids(), edges);

    // A new curve crossing curve 1's right half only splits that edge: every
    // other node and edge keeps its id.
    Net grown = n;
    grown.add(4, line({{5, 0, -3}, {5, 0, 3}}));
    grown.build();
    const auto grown_nodes = grown.node_ids();
    const auto grown_edges = grown.edge_ids();
    for (const auto id : nodes) EXPECT_TRUE(grown_nodes.contains(id));
    std::size_t kept = 0;
    for (const auto id : edges) kept += grown_edges.contains(id) ? 1U : 0U;
    EXPECT_EQ(kept, edges.size() - 1);
    EXPECT_EQ(grown.net.edges.size(), n.net.edges.size() + 3); // split edge -> 2, plus curve 4's two halves
}

// Far from the origin (float ulp ~ 0.004 m at 5e4) chords must follow the
// curve, not rounding noise: no phantom crossings, the same network.
TEST(SplineNetwork, LargeCoordinatesFindNoPhantomCrossings) {
    for (const float b : {1.0e3F, 5.0e4F, 1.0e5F, 5.0e5F}) {
        Net n;
        n.add(1, line({{b, 0, b}, {b + 50, 0, b + 40}, {b + 100, 0, b}, {b + 150, 0, b + 40}},
                      sp::SplineKind::centripetal_catmull_rom));
        n.add(2, line({{b + 75, 0, b - 10}, {b + 75, 0, b + 60}}));
        EXPECT_TRUE(sp::self_crossings_xz(n.curves[0]).empty()) << "b = " << b;
        EXPECT_EQ(sp::crossings_xz(n.curves[0], n.curves[1]).size(), 1U) << "b = " << b;
        n.build();
        EXPECT_EQ(n.net.nodes.size(), 5U) << "b = " << b;
        EXPECT_EQ(n.net.edges.size(), 4U) << "b = " << b;
    }
}

TEST(SplineNetwork, RejectsBadInput) {
    const sp::Curve c = line({{0, 0, 0}, {1, 0, 0}});
    std::vector<sp::NetworkCurve> in = {{1, &c}, {1, &c}};
    EXPECT_EQ(sp::build_network(in).error().message, "curve id 1 is used twice");
    in = {{1, &c}};
    EXPECT_EQ(sp::build_network(in, {.snap_radius = 0.0F}).error().message, "snap_radius: expected a number > 0");
    const sp::Curve empty;
    in = {{5, &empty}};
    EXPECT_EQ(sp::build_network(in).error().message, "curve 5: empty");
    EXPECT_EQ(sp::node_kind_name(sp::NodeKind::tee), "tee");
}

} // namespace
} // namespace quat::test
