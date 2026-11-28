// Curve networks (ROADMAP G18.2): snapping, T-junctions, crossings, edges,
// node classification and stable ids. See include/engine/spline/network.hpp.

#include "engine/spline/network.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>

#include "engine/spline/queries.hpp"

namespace engine::spline {

namespace {

constexpr std::uint64_t k_end_tag = 0x454E44ULL;   // "END"
constexpr std::uint64_t k_cross_tag = 0x58ULL;     // "X"
constexpr std::uint64_t k_loop_tag = 0x4C4F4FULL;  // "LOO"
constexpr std::uint64_t k_edge_tag = 0x45444745ULL; // "EDGE"

[[nodiscard]] std::uint64_t splitmix(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] std::uint64_t hash_ids(std::initializer_list<std::uint64_t> parts) {
    std::uint64_t h = 0x51A11E5ULL;
    for (const std::uint64_t p : parts) h = splitmix(h ^ splitmix(p));
    return h;
}

[[nodiscard]] float xz_distance2(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

struct Input {
    std::uint32_t id = 0;
    std::uint32_t index = 0; // in the caller's span
    const Curve*  curve = nullptr;
    Bounds        box{};     // XZ bounds grown by the snap radius
};

struct Draft {
    std::uint64_t          id = 0;
    std::vector<glm::vec3> points; // joined positions (mean = node position)
};

struct Split {
    float         t = 0.0F;
    std::uint32_t node = 0; // draft index
};

struct Endpoint {
    std::uint32_t curve = 0; // input index (sorted order)
    std::uint32_t end = 0;   // 0 = start, 1 = end
    glm::vec3     position{0.0F};
};

class Builder {
public:
    Builder(std::vector<Input> inputs, const NetworkOptions& o) : in_(std::move(inputs)), o_(o) {
        splits_.resize(in_.size());
    }

    CurveNetwork run() {
        endpoints();
        tees();
        crossings();
        loops();
        return finish();
    }

private:
    [[nodiscard]] bool near_box(const Bounds& b, const glm::vec3& p) const {
        return p.x >= b.min.x && p.x <= b.max.x && p.z >= b.min.z && p.z <= b.max.z;
    }

    [[nodiscard]] std::uint32_t add_draft(std::uint64_t id) {
        drafts_.push_back({id, {}});
        return static_cast<std::uint32_t>(drafts_.size() - 1);
    }

    // Endpoints within the snap radius join one node (union-find; the root
    // is always the lowest index, so clusters do not depend on visit order).
    void endpoints() {
        for (std::uint32_t k = 0; k < in_.size(); ++k) {
            const Curve& c = *in_[k].curve;
            if (c.closed()) continue;
            ends_.push_back({k, 0, c.position(0.0F)});
            ends_.push_back({k, 1, c.position(c.max_param())});
        }
        std::vector<std::uint32_t> parent(ends_.size());
        std::iota(parent.begin(), parent.end(), 0U);
        auto root = [&parent](std::uint32_t i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];
            return i;
        };
        const float r2 = o_.snap_radius * o_.snap_radius;
        auto join = [&](std::uint32_t i, std::uint32_t j) {
            const std::uint32_t a = root(i);
            const std::uint32_t b = root(j);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        };
        std::vector<std::int64_t> first_end(in_.size(), -1); // index of a curve's start in ends_
        for (std::uint32_t i = 0; i < ends_.size(); ++i) {
            if (ends_[i].end == 0) first_end[ends_[i].curve] = i;
        }
        for (std::uint32_t i = 0; i < ends_.size(); ++i) {
            for (std::uint32_t j = i + 1; j < ends_.size(); ++j) {
                if (xz_distance2(ends_[i].position, ends_[j].position) <= r2) join(i, j);
            }
            // Within the radius of another curve near (within the radius
            // along it of) one of its ends: it joins that end, as tees()
            // leaves such a point to the endpoint clusters.
            for (std::uint32_t k = 0; k < in_.size(); ++k) {
                if (k == ends_[i].curve || first_end[k] < 0 || !near_box(in_[k].box, ends_[i].position)) continue;
                const Curve& c = *in_[k].curve;
                const auto cp = closest_point(c, ends_[i].position, true);
                if (!cp || cp->gap > o_.snap_radius) continue;
                const auto base = static_cast<std::uint32_t>(first_end[k]);
                if (cp->distance <= o_.snap_radius) join(i, base);
                if (c.length() - cp->distance <= o_.snap_radius) join(i, base + 1);
            }
        }
        end_node_.assign(ends_.size(), 0);
        std::vector<std::int64_t> node_of_root(ends_.size(), -1);
        for (std::uint32_t i = 0; i < ends_.size(); ++i) {
            const std::uint32_t r = root(i);
            if (node_of_root[r] < 0) {
                // Named after its first member: the lowest (curve id, end).
                node_of_root[r] = add_draft(hash_ids({k_end_tag, in_[ends_[r].curve].id, ends_[r].end}));
                end_drafts_.push_back(static_cast<std::uint32_t>(node_of_root[r]));
            }
            const auto node = static_cast<std::uint32_t>(node_of_root[r]);
            end_node_[i] = node;
            drafts_[node].points.push_back(ends_[i].position);
            const Curve& c = *in_[ends_[i].curve].curve;
            splits_[ends_[i].curve].push_back({ends_[i].end == 0 ? 0.0F : c.max_param(), node});
        }
    }

    // An endpoint on another curve's interior splits that curve (a T).
    void tees() {
        const float r = o_.snap_radius;
        for (std::uint32_t i = 0; i < ends_.size(); ++i) {
            const Endpoint& e = ends_[i];
            for (std::uint32_t k = 0; k < in_.size(); ++k) {
                if (k == e.curve || !near_box(in_[k].box, e.position)) continue;
                const Curve& c = *in_[k].curve;
                const auto cp = closest_point(c, e.position, true);
                if (!cp || cp->gap > r) continue;
                if (!c.closed() && (cp->distance <= r || c.length() - cp->distance <= r)) continue; // an end
                splits_[k].push_back({cp->param, end_node_[i]});
                drafts_[end_node_[i]].points.push_back(cp->position);
            }
        }
    }

    [[nodiscard]] std::int64_t node_near(const std::vector<std::uint32_t>& nodes, const glm::vec3& p) const {
        const float r2 = o_.snap_radius * o_.snap_radius;
        for (const std::uint32_t n : nodes) {
            for (const glm::vec3& q : drafts_[n].points) {
                if (xz_distance2(p, q) <= r2) return n;
            }
        }
        return -1;
    }

    void crossing(std::uint32_t a, std::uint32_t b, const std::vector<Crossing>& hits) {
        for (std::size_t k = 0; k < hits.size(); ++k) {
            const Crossing& h = hits[k];
            std::int64_t node = node_near(end_drafts_, h.position_a);
            if (node < 0) node = node_near(cross_drafts_, h.position_a);
            if (node < 0) {
                node = add_draft(hash_ids({k_cross_tag, in_[a].id, in_[b].id, k}));
                cross_drafts_.push_back(static_cast<std::uint32_t>(node));
            }
            const auto n = static_cast<std::uint32_t>(node);
            drafts_[n].points.push_back(h.position_a);
            drafts_[n].points.push_back(h.position_b);
            splits_[a].push_back({h.param_a, n});
            splits_[b].push_back({h.param_b, n});
        }
    }

    void crossings() {
        CrossingOptions co;
        co.tolerance = o_.crossing_tolerance;
        co.merge_distance = std::min(o_.snap_radius, 1.0e-3F);
        for (std::uint32_t a = 0; a < in_.size(); ++a) {
            crossing(a, a, self_crossings_xz(*in_[a].curve, co));
            for (std::uint32_t b = a + 1; b < in_.size(); ++b) {
                const Bounds& p = in_[a].box;
                const Bounds& q = in_[b].box;
                if (p.max.x < q.min.x || q.max.x < p.min.x || p.max.z < q.min.z || q.max.z < p.min.z) continue;
                crossing(a, b, crossings_xz(*in_[a].curve, *in_[b].curve, co));
            }
        }
    }

    // A closed curve that meets nothing still needs a node to hang its edge on.
    void loops() {
        for (std::uint32_t k = 0; k < in_.size(); ++k) {
            if (!splits_[k].empty()) continue;
            const std::uint32_t n = add_draft(hash_ids({k_loop_tag, in_[k].id}));
            drafts_[n].points.push_back(in_[k].curve->position(0.0F));
            splits_[k].push_back({0.0F, n});
        }
    }

    // Arc length between parameters along a curve (the shorter way on a loop).
    [[nodiscard]] static float along(const Curve& c, float t0, float t1) {
        const float d = std::abs(c.distance_at(t0) - c.distance_at(t1));
        return c.closed() ? std::min(d, c.length() - d) : d;
    }

    // Sorted splits with repeats of one node close together removed (a T and a
    // crossing found at the same junction). Self-crossings keep both visits.
    [[nodiscard]] std::vector<Split> clean(std::uint32_t k) const {
        const Curve& c = *in_[k].curve;
        std::vector<Split> s = splits_[k];
        std::stable_sort(s.begin(), s.end(), [](const Split& p, const Split& q) { return p.t < q.t; });
        std::vector<Split> out;
        for (const Split& x : s) {
            bool dup = false;
            for (const Split& y : out) {
                if (y.node == x.node && along(c, x.t, y.t) <= 2.0F * o_.snap_radius) {
                    dup = true;
                    break;
                }
            }
            if (!dup) out.push_back(x);
        }
        return out;
    }

    [[nodiscard]] static bool usable(const glm::vec3& d) { return d.x * d.x + d.z * d.z > 1.0e-12F; }

    // First non-degenerate XZ chord of a segment's control polygon, leaving its
    // start (forward) or its end (backward): the one-sided tangent direction.
    [[nodiscard]] static glm::vec3 polygon_direction(const Segment& seg, bool forward) {
        for (int k = 1; k < 4; ++k) {
            const glm::vec3 d = forward ? seg.p[k] - seg.p[0] : seg.p[3 - k] - seg.p[3];
            if (usable(d)) return d;
        }
        return glm::vec3(0.0F);
    }

    // Unit XZ direction leaving the curve at t towards increasing (sign > 0)
    // or decreasing t. Doubled knots (stroke corners) make zero-length
    // segments where the derivative vanishes over a whole parameter interval,
    // so past a small step the direction comes from the next segment along
    // that is not degenerate.
    [[nodiscard]] static glm::vec2 leaving(const Curve& c, float t, float sign) {
        glm::vec3 d = c.derivative(t) * sign;
        if (!usable(d)) d = c.position(t + sign * 1.0e-3F) - c.position(t);
        if (!usable(d)) {
            const bool  forward = sign > 0.0F;
            const auto  n = static_cast<std::ptrdiff_t>(c.segment_count());
            std::size_t seg = 0;
            float       u = 0.0F;
            c.locate(t, seg, u);
            auto k = static_cast<std::ptrdiff_t>(seg);
            if (forward && u >= 1.0F) ++k;
            if (!forward && u <= 0.0F) --k;
            for (std::ptrdiff_t step = 0; step < n && !usable(d); ++step, k += forward ? 1 : -1) {
                if (k < 0 || k >= n) {
                    if (!c.closed()) break;
                    k = (k % n + n) % n;
                }
                d = polygon_direction(c.segments()[static_cast<std::size_t>(k)], forward);
            }
        }
        const float len = std::sqrt(d.x * d.x + d.z * d.z);
        return len > 0.0F ? glm::vec2(d.x / len, d.z / len) : glm::vec2(1.0F, 0.0F);
    }

    CurveNetwork finish() {
        // Edges in draft-node terms first.
        struct EdgeDraft {
            std::uint32_t curve;
            float         t0, t1;
            std::uint32_t a, b;
        };
        std::vector<EdgeDraft> edges;
        for (std::uint32_t k = 0; k < in_.size(); ++k) {
            const Curve& c = *in_[k].curve;
            const std::vector<Split> s = clean(k);
            for (std::size_t i = 0; i + 1 < s.size(); ++i) edges.push_back({k, s[i].t, s[i + 1].t, s[i].node, s[i + 1].node});
            if (c.closed()) edges.push_back({k, s.back().t, s.front().t + c.max_param(), s.back().node, s.front().node});
        }
        // Nodes sorted by id; drafts that ended up without edges are dropped.
        std::vector<std::uint32_t> order(drafts_.size());
        std::iota(order.begin(), order.end(), 0U);
        std::stable_sort(order.begin(), order.end(),
                         [this](std::uint32_t p, std::uint32_t q) { return drafts_[p].id < drafts_[q].id; });
        std::vector<std::uint32_t> remap(drafts_.size(), 0);
        CurveNetwork net;
        for (const std::uint32_t d : order) {
            remap[d] = static_cast<std::uint32_t>(net.nodes.size());
            NetworkNode n;
            n.id = drafts_[d].id;
            glm::vec3 sum(0.0F);
            for (const glm::vec3& p : drafts_[d].points) sum += p;
            n.position = sum / static_cast<float>(std::max<std::size_t>(drafts_[d].points.size(), 1));
            net.nodes.push_back(std::move(n));
        }
        std::stable_sort(edges.begin(), edges.end(), [this](const EdgeDraft& p, const EdgeDraft& q) {
            return in_[p.curve].id < in_[q.curve].id || (in_[p.curve].id == in_[q.curve].id && p.t0 < q.t0);
        });
        for (const EdgeDraft& e : edges) {
            const Curve& c = *in_[e.curve].curve;
            NetworkEdge out;
            out.curve_id = in_[e.curve].id;
            out.curve_index = in_[e.curve].index;
            out.t0 = e.t0;
            out.t1 = e.t1;
            const float d0 = c.distance_at(e.t0);
            out.length = e.t1 > c.max_param() ? c.length() - d0 + c.distance_at(e.t1 - c.max_param())
                                              : c.distance_at(e.t1) - d0;
            out.node_a = remap[e.a];
            out.node_b = remap[e.b];
            std::uint64_t dup = 0;
            for (const NetworkEdge& prev : net.edges) {
                if (prev.curve_id == out.curve_id && prev.node_a == out.node_a && prev.node_b == out.node_b) ++dup;
            }
            out.id = hash_ids({k_edge_tag, out.curve_id, net.nodes[out.node_a].id, net.nodes[out.node_b].id, dup});
            const auto index = static_cast<std::uint32_t>(net.edges.size());
            net.nodes[out.node_a].ends.push_back({index, true, leaving(c, e.t0, 1.0F)});
            net.nodes[out.node_b].ends.push_back({index, false, leaving(c, e.t1, -1.0F)});
            net.edges.push_back(out);
        }
        std::erase_if(net.nodes, [](const NetworkNode& n) { return n.ends.empty(); });
        // Re-point edges after dropping empty drafts.
        for (std::uint32_t i = 0; i < net.nodes.size(); ++i) {
            for (const NodeEnd& end : net.nodes[i].ends) {
                NetworkEdge& e = net.edges[end.edge];
                (end.at_start ? e.node_a : e.node_b) = i;
            }
        }
        for (NetworkNode& n : net.nodes) classify(n);
        return net;
    }

    void classify(NetworkNode& n) const {
        // Counter-clockwise seen from above: angle of (x, -z) from +X.
        auto half = [](const glm::vec2& d) { return (-d.y < 0.0F || (-d.y == 0.0F && d.x < 0.0F)) ? 1 : 0; };
        std::stable_sort(n.ends.begin(), n.ends.end(), [&half](const NodeEnd& p, const NodeEnd& q) {
            const int hp = half(p.direction);
            const int hq = half(q.direction);
            if (hp != hq) return hp < hq;
            const float cr = p.direction.x * -q.direction.y - -p.direction.y * q.direction.x;
            if (cr != 0.0F) return cr > 0.0F;
            return p.edge < q.edge || (p.edge == q.edge && p.at_start && !q.at_start);
        });
        switch (n.ends.size()) {
        case 1: n.kind = NodeKind::end; break;
        case 2: {
            const glm::vec2 a = n.ends[0].direction;
            const glm::vec2 b = n.ends[1].direction;
            n.kind = a.x * b.x + a.y * b.y <= -o_.straight_cos ? NodeKind::straight : NodeKind::bend;
            break;
        }
        case 3: n.kind = NodeKind::tee; break;
        case 4: n.kind = NodeKind::cross; break;
        default: n.kind = NodeKind::n_way; break;
        }
    }

    std::vector<Input>              in_;
    NetworkOptions                  o_;
    std::vector<Draft>              drafts_;
    std::vector<std::vector<Split>> splits_;
    std::vector<Endpoint>           ends_;
    std::vector<std::uint32_t>      end_node_;
    std::vector<std::uint32_t>      end_drafts_;
    std::vector<std::uint32_t>      cross_drafts_;
};

} // namespace

std::string_view node_kind_name(NodeKind kind) {
    switch (kind) {
    case NodeKind::end: return "end";
    case NodeKind::straight: return "straight";
    case NodeKind::bend: return "bend";
    case NodeKind::tee: return "tee";
    case NodeKind::cross: return "cross";
    case NodeKind::n_way: return "n_way";
    }
    return "end";
}

const NetworkNode* CurveNetwork::find_node(std::uint64_t id) const {
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), id,
                                     [](const NetworkNode& n, std::uint64_t v) { return n.id < v; });
    return it != nodes.end() && it->id == id ? &*it : nullptr;
}

const NetworkEdge* CurveNetwork::find_edge(std::uint64_t id) const {
    for (const NetworkEdge& e : edges) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::expected<CurveNetwork, core::Error> build_network(std::span<const NetworkCurve> curves,
                                                       const NetworkOptions& options) {
    if (!std::isfinite(options.snap_radius) || options.snap_radius <= 0.0F) {
        return std::unexpected(core::make_error("snap_radius: expected a number > 0"));
    }
    std::vector<Input> in;
    in.reserve(curves.size());
    for (std::uint32_t i = 0; i < curves.size(); ++i) {
        const NetworkCurve& c = curves[i];
        if (c.curve == nullptr || c.curve->empty()) {
            return std::unexpected(core::make_error("curve " + std::to_string(c.id) + ": empty"));
        }
        Bounds b = c.curve->bounds();
        b.min -= glm::vec3(options.snap_radius);
        b.max += glm::vec3(options.snap_radius);
        in.push_back({c.id, i, c.curve, b});
    }
    std::stable_sort(in.begin(), in.end(), [](const Input& a, const Input& b) { return a.id < b.id; });
    for (std::size_t i = 1; i < in.size(); ++i) {
        if (in[i].id == in[i - 1].id) {
            return std::unexpected(core::make_error("curve id " + std::to_string(in[i].id) + " is used twice"));
        }
    }
    return Builder(std::move(in), options).run();
}

} // namespace engine::spline
