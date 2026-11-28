#include "geometry/skeleton_wavefront.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "geometry/det_math.hpp"

namespace engine::geometry::skeleton_detail {

namespace {

double cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

// Directions within this angle (radians) count as equal when pairing the
// chains of an event.
constexpr double kAngleTolerance = 1e-9;

double distance_to_segment(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b) {
    const glm::dvec2 ab = b - a;
    const double len2 = glm::dot(ab, ab);
    double s = 0.0;
    if (len2 > 0.0) {
        s = std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
    }
    return glm::length(p - (a + ab * s));
}

} // namespace

std::expected<glm::dvec2, core::Error> Wavefront::velocity(std::uint32_t left, std::uint32_t right,
                                                           bool& ridge) const {
    ridge = false;
    const MovingLine& l = lines_[left];
    const MovingLine& r = lines_[right];
    const double dot = glm::dot(l.normal, r.normal);
    const double det = cross(l.normal, r.normal);
    constexpr double parallel = 1e-9;
    if (l.weight == r.weight && dot > -1.0 + parallel) {
        // Equal speeds: along the angle bisector, |v| = w / cos(half angle).
        // Stable for nearly collinear edges, where Cramer's rule is not.
        return (l.normal + r.normal) * (l.weight / (1.0 + dot));
    }
    if (std::fabs(det) > parallel) {
        return glm::dvec2{(l.weight * r.normal.y - l.normal.y * r.weight) / det,
                          (l.normal.x * r.weight - r.normal.x * l.weight) / det};
    }
    if (dot > 0.0) {
        return std::unexpected(core::make_error(std::format(
            "straight skeleton: edges {} and {} became adjacent and parallel with different weights ({} and "
            "{}); the weighted skeleton is ambiguous there (Biedl et al. 2015) - give parallel edges that can "
            "meet the same weight",
            left, right, l.weight, r.weight)));
    }
    // Antiparallel and coincident: the region between them has collapsed to
    // a ridge. The vertex stays put; the loop closes as a ridge.
    ridge = true;
    return glm::dvec2{0.0};
}

std::expected<std::uint32_t, core::Error> Wavefront::make_vertex(glm::dvec2 position, double t,
                                                                 std::uint32_t left, std::uint32_t right,
                                                                 std::uint32_t node) {
    bool ridge = false;
    auto v = velocity(left, right, ridge);
    if (!v) {
        return std::unexpected(v.error());
    }
    WaveVertex w;
    w.origin = position;
    w.t0 = t;
    w.velocity = *v;
    w.left_line = left;
    w.right_line = right;
    w.node = node;
    // Reflex: the boundary turns right (clockwise) at the vertex. Straight
    // vertices (collinear edges, after a notch closed) can hit the opposite
    // front too, when the strip between them collapses: they are tested for
    // split events as well.
    const double turn = cross(lines_[left].dir, lines_[right].dir);
    w.reflex = !ridge && (turn < 0.0 || (std::fabs(turn) <= 1e-9 &&
                                         glm::dot(lines_[left].dir, lines_[right].dir) > 0.0));
    vertices_.push_back(w);
    return static_cast<std::uint32_t>(vertices_.size() - 1);
}

std::uint32_t Wavefront::add_node(glm::dvec2 position, double t) {
    out_->nodes.push_back(SkeletonNode{position, t});
    out_->max_time = std::max(out_->max_time, t);
    return static_cast<std::uint32_t>(out_->nodes.size() - 1);
}

void Wavefront::add_arc(std::uint32_t from, std::uint32_t to, std::uint32_t left, std::uint32_t right) {
    if (from != to) {
        out_->arcs.push_back(SkeletonArc{from, to, left, right});
    }
}

void Wavefront::add_ridge(std::uint32_t from, std::uint32_t to, std::uint32_t line_a, std::uint32_t line_b) {
    if (from == to) {
        return;
    }
    // A ridge separates two antiparallel fronts that met; each face lies
    // behind its own front, towards -normal from the ridge.
    const glm::dvec2 d = out_->nodes[to].position - out_->nodes[from].position;
    const glm::dvec2 left_perp{-d.y, d.x};
    const double side_a = glm::dot(left_perp, -lines_[line_a].normal);
    const double side_b = glm::dot(left_perp, -lines_[line_b].normal);
    if (side_b > side_a) {
        std::swap(line_a, line_b);
    }
    add_arc(from, to, line_a, line_b);
}

void Wavefront::kill(std::uint32_t v, std::uint32_t node) {
    // A vertex's path has the face of its incoming edge on the left and that
    // of its outgoing edge on the right (convex and reflex alike).
    WaveVertex& w = vertices_[v];
    w.alive = false;
    add_arc(w.node, node, w.left_line, w.right_line);
}

bool Wavefront::edge_is_current(std::uint32_t v) const {
    const WaveVertex& w = vertices_[v];
    if (w.split_edge == kNone) {
        return true;
    }
    const WaveVertex& e = vertices_[w.split_edge];
    return e.alive && e.next == w.split_edge_next && vertices_[e.next].alive;
}

void Wavefront::compute_edge_event(std::uint32_t v) {
    WaveVertex& u = vertices_[v];
    u.edge_time = kNever;
    const WaveVertex& w = vertices_[u.next];
    if (u.next == v) {
        return;
    }
    const glm::dvec2 d = lines_[u.right_line].dir;
    const double s = glm::dot(w.at(now_) - u.at(now_), d);
    const double ds = glm::dot(w.velocity - u.velocity, d);
    if (ds < 0.0) {
        u.edge_time = now_ + std::max(s, 0.0) / -ds;
    } else if (s <= tol_.distance) {
        u.edge_time = now_; // already collapsed (zero-length edge from an event)
    }
}

double Wavefront::split_time_against(std::uint32_t v, std::uint32_t edge_start) const {
    const WaveVertex& w = vertices_[v];
    const WaveVertex& a = vertices_[edge_start];
    if (!a.alive || edge_start == v || a.next == v) {
        return kNever;
    }
    const WaveVertex& b = vertices_[a.next];
    const MovingLine& e = lines_[a.right_line];
    const double f = glm::dot(e.normal, w.at(now_) - e.point) - e.weight * now_;
    if (f < -tol_.distance) {
        return kNever; // behind the edge's front
    }
    const double rate = glm::dot(e.normal, w.velocity) - e.weight;
    if (rate >= 0.0) {
        return kNever;
    }
    const double t = now_ + std::max(f, 0.0) / -rate;
    const glm::dvec2 x = w.at(t);
    const glm::dvec2 pa = a.at(t);
    const glm::dvec2 pb = b.at(t);
    const double len = glm::dot(pb - pa, e.dir);
    const double s = glm::dot(x - pa, e.dir);
    if (len < -tol_.distance || s < -tol_.distance || s > len + tol_.distance) {
        return kNever;
    }
    return t;
}

void Wavefront::compute_split_event(std::uint32_t v) {
    WaveVertex& w = vertices_[v];
    w.split_time = kNever;
    w.split_edge = kNone;
    w.split_edge_next = kNone;
    if (!w.reflex) {
        return;
    }
    for (std::uint32_t e = 0; e < vertices_.size(); ++e) {
        const double t = split_time_against(v, e);
        if (t < w.split_time) {
            w.split_time = t;
            w.split_edge = e;
            w.split_edge_next = vertices_[e].next;
        }
    }
}

std::expected<void, core::Error> Wavefront::resolve(glm::dvec2 x, double t, std::vector<std::uint32_t>& created,
                                                   bool& no_change) {
    no_change = false;
    // Vertices that arrive at x, in id order (determinism).
    std::vector<std::uint32_t> arriving;
    for (std::uint32_t v = 0; v < vertices_.size(); ++v) {
        if (vertices_[v].alive && glm::length(vertices_[v].at(t) - x) <= tol_.distance) {
            arriving.push_back(v);
        }
    }
    if (arriving.empty()) {
        return {};
    }
    auto is_arriving = [&](std::uint32_t v) { return std::binary_search(arriving.begin(), arriving.end(), v); };

    // Chains: maximal runs of consecutive arriving vertices; the edges
    // between them have collapsed.
    std::vector<Chain> chains;
    std::vector<bool> chained(arriving.size(), false);
    for (std::size_t i = 0; i < arriving.size(); ++i) {
        const std::uint32_t start = arriving[i];
        if (is_arriving(vertices_[start].prev)) {
            continue;
        }
        std::uint32_t end = start;
        while (is_arriving(vertices_[end].next)) {
            end = vertices_[end].next;
        }
        chains.push_back(Chain{vertices_[start].left_line, vertices_[end].right_line, vertices_[start].prev,
                               vertices_[end].next});
    }
    // Wavefront edges passing through x (split events): a chain without
    // vertices whose edge is cut in two.
    std::size_t split_chains = 0;
    for (std::uint32_t a = 0; a < vertices_.size(); ++a) {
        const WaveVertex& va = vertices_[a];
        if (!va.alive || is_arriving(a) || is_arriving(va.next)) {
            continue;
        }
        const glm::dvec2 pa = va.at(t);
        const glm::dvec2 pb = vertices_[va.next].at(t);
        if (distance_to_segment(x, pa, pb) > tol_.distance) {
            continue;
        }
        const glm::dvec2 d = lines_[va.right_line].dir;
        const double s = glm::dot(x - pa, d);
        const double len = glm::dot(pb - pa, d);
        if (s <= tol_.distance || s >= len - tol_.distance) {
            continue;
        }
        chains.push_back(Chain{va.right_line, va.right_line, a, va.next});
        ++split_chains;
    }

    const std::uint32_t node = add_node(x, t);
    for (const std::uint32_t v : arriving) {
        kill(v, node); // whole loops collapsing to x die here too
    }
    if (chains.empty()) {
        return {};
    }
    // Generalised intersection event (Kelly & Wonka 2011): every chain's
    // incoming edge continues as some chain's outgoing edge. Near x the
    // polygon interior lies clockwise of the reversed incoming direction, so
    // the incoming edge pairs with the first outgoing direction met rotating
    // clockwise from it (equal directions first: two fronts that met
    // head-on form a ridge). With disjoint wedges this is the next chain
    // counter-clockwise; it stays right when wedges degenerate at the event
    // (a cross-shaped footprint collapsing onto its centre).
    const std::size_t k = chains.size();
    std::vector<double> out_angle(k);
    for (std::size_t j = 0; j < k; ++j) {
        const glm::dvec2 d = lines_[chains[j].out_line].dir;
        out_angle[j] = det::atan2(d.y, d.x);
    }
    std::vector<bool> taken(k, false);
    for (std::size_t i = 0; i < k; ++i) {
        const glm::dvec2 back = -lines_[chains[i].in_line].dir;
        const double from = det::atan2(back.y, back.x);
        std::size_t best = k;
        double best_turn = 0.0;
        for (std::size_t j = 0; j < k; ++j) {
            if (taken[j]) {
                continue;
            }
            double turn = from - out_angle[j]; // clockwise rotation from `back`
            if (turn < -kAngleTolerance) {
                turn += det::two_pi;
            }
            turn = std::max(turn, 0.0);
            if (best == k || turn < best_turn - kAngleTolerance) {
                best = j;
                best_turn = turn;
            }
        }
        taken[best] = true;
        const Chain& in = chains[i];
        const Chain& out = chains[best];
        auto nv = make_vertex(x, t, in.in_line, out.out_line, node);
        if (!nv) {
            return std::unexpected(nv.error());
        }
        WaveVertex& n = vertices_[*nv];
        n.prev = in.in_far;
        n.next = out.out_far;
        vertices_[in.in_far].next = *nv;
        vertices_[out.out_far].prev = *nv;
        created.push_back(*nv);
    }
    if (split_chains == 0) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> before_lines;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> after_lines;
        for (const std::uint32_t v : arriving) {
            before_lines.emplace_back(vertices_[v].left_line, vertices_[v].right_line);
        }
        for (std::size_t i = created.size() - k; i < created.size(); ++i) {
            after_lines.emplace_back(vertices_[created[i]].left_line, vertices_[created[i]].right_line);
        }
        std::sort(before_lines.begin(), before_lines.end());
        std::sort(after_lines.begin(), after_lines.end());
        no_change = before_lines == after_lines;
    }
    return {};
}

std::expected<void, core::Error> Wavefront::close_degenerate_loops(std::vector<std::uint32_t>& created, double t) {
    for (const std::uint32_t v : created) {
        WaveVertex& w = vertices_[v];
        if (!w.alive) {
            continue;
        }
        if (w.next == v) {
            w.alive = false; // a lone vertex: its loop has shrunk to this point
            continue;
        }
        const std::uint32_t o = w.next;
        if (vertices_[o].next != v) {
            continue;
        }
        // Two vertices joined by two edges: the loop has collapsed to the
        // segment between them (a ridge).
        WaveVertex& other = vertices_[o];
        // The far end reuses a node already at that point and time (the
        // other vertex was usually born there in this batch).
        std::uint32_t other_node = w.node;
        const glm::dvec2 far = other.at(t);
        if (glm::length(far - w.at(t)) > tol_.distance) {
            const SkeletonNode& born = out_->nodes[other.node];
            other_node = glm::length(born.position - far) <= tol_.distance &&
                                 std::fabs(born.time - t) <= tol_.time
                             ? other.node
                             : add_node(far, t);
        }
        kill(o, other_node);
        w.alive = false;
        add_ridge(w.node, other_node, w.left_line, w.right_line);
    }
    return {};
}

void Wavefront::refresh_events(const std::vector<std::uint32_t>& created) {
    std::vector<std::uint32_t> changed; // start vertices of edges that changed
    for (const std::uint32_t v : created) {
        if (vertices_[v].alive) {
            changed.push_back(v);
            changed.push_back(vertices_[v].prev);
        }
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (const std::uint32_t v : changed) {
        if (vertices_[v].alive) {
            compute_edge_event(v);
        }
    }
    for (std::uint32_t v = 0; v < vertices_.size(); ++v) {
        WaveVertex& w = vertices_[v];
        if (!w.alive || !w.reflex) {
            continue;
        }
        const bool fresh = std::binary_search(changed.begin(), changed.end(), v) &&
                           std::find(created.begin(), created.end(), v) != created.end();
        if (fresh || !edge_is_current(v)) {
            compute_split_event(v);
            continue;
        }
        for (const std::uint32_t e : changed) {
            if (!vertices_[e].alive) {
                continue;
            }
            const double t = split_time_against(v, e);
            if (t < w.split_time) {
                w.split_time = t;
                w.split_edge = e;
                w.split_edge_next = vertices_[e].next;
            }
        }
    }
}

std::expected<void, core::Error> Wavefront::run(const std::vector<std::vector<glm::dvec2>>& rings,
                                                const std::vector<double>& weights, StraightSkeleton& out) {
    out_ = &out;
    lines_.clear();
    vertices_.clear();
    now_ = 0.0;

    glm::dvec2 lo{std::numeric_limits<double>::max()};
    glm::dvec2 hi{std::numeric_limits<double>::lowest()};
    double min_weight = std::numeric_limits<double>::max();
    std::uint32_t edge = 0;
    for (const auto& ring : rings) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const glm::dvec2 a = ring[i];
            const glm::dvec2 b = ring[(i + 1) % ring.size()];
            MovingLine line;
            line.point = a;
            line.dir = glm::normalize(b - a);
            line.normal = {-line.dir.y, line.dir.x};
            line.weight = weights[edge++];
            if (line.weight > 0.0) {
                min_weight = std::min(min_weight, line.weight);
            }
            lines_.push_back(line);
            lo = glm::min(lo, a);
            hi = glm::max(hi, a);
            out.nodes.push_back(SkeletonNode{a, 0.0});
        }
    }
    const double extent = glm::length(hi - lo);
    tol_.distance = 1e-9 * extent;
    tol_.time = tol_.distance / std::min(min_weight, 1.0);

    std::uint32_t base = 0;
    for (const auto& ring : rings) {
        const auto n = static_cast<std::uint32_t>(ring.size());
        for (std::uint32_t i = 0; i < n; ++i) {
            auto v = make_vertex(ring[i], 0.0, base + (i + n - 1) % n, base + i, base + i);
            if (!v) {
                return std::unexpected(v.error());
            }
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            vertices_[base + i].prev = base + (i + n - 1) % n;
            vertices_[base + i].next = base + (i + 1) % n;
        }
        base += n;
    }
    for (std::uint32_t v = 0; v < vertices_.size(); ++v) {
        compute_edge_event(v);
        compute_split_event(v);
    }

    const std::size_t limit = 64 * vertices_.size() + 1024;
    std::vector<double> split_floor(vertices_.size(), -1.0);
    for (std::size_t iteration = 0;; ++iteration) {
        if (iteration > limit) {
            return std::unexpected(core::make_error(
                std::format("straight skeleton: no convergence after {} events (degenerate input?)", limit)));
        }
        double t_min = kNever;
        bool any_alive = false;
        for (const WaveVertex& w : vertices_) {
            if (w.alive) {
                any_alive = true;
                t_min = std::min({t_min, w.edge_time, w.split_time});
            }
        }
        if (!any_alive) {
            break;
        }
        if (t_min == kNever) {
            return std::unexpected(core::make_error(std::format(
                "straight skeleton: the wavefront stalled at time {}: part of the polygon is enclosed only by "
                "weight-0 edges, so no moving edge ever sweeps it",
                now_)));
        }
        // Every event within the time tolerance, as (time, point, vertex).
        std::vector<Candidate> batch;
        for (std::uint32_t v = 0; v < vertices_.size(); ++v) {
            const WaveVertex& w = vertices_[v];
            if (!w.alive) {
                continue;
            }
            if (w.edge_time <= t_min + tol_.time) {
                const double t = std::max(w.edge_time, now_);
                batch.push_back(Candidate{t, (w.at(t) + vertices_[w.next].at(t)) * 0.5, v});
            }
            if (w.split_time <= t_min + tol_.time) {
                const double t = std::max(w.split_time, now_);
                batch.push_back(Candidate{t, w.at(t), v});
            }
        }
        std::sort(batch.begin(), batch.end(), [](const Candidate& a, const Candidate& b) {
            return a.time != b.time ? a.time < b.time : a.vertex < b.vertex;
        });
        // One generalised event per point.
        std::vector<Candidate> clusters;
        for (const Candidate& c : batch) {
            const bool joined = std::any_of(clusters.begin(), clusters.end(), [&](const Candidate& k) {
                return glm::length(k.point - c.point) <= 2.0 * tol_.distance;
            });
            if (!joined) {
                clusters.push_back(c);
            }
        }
        std::vector<std::uint32_t> created;
        double t_batch = now_;
        for (const Candidate& c : clusters) {
            if (!vertices_[c.vertex].alive) {
                continue;
            }
            const std::size_t before = created.size();
            bool no_change = false;
            if (auto r = resolve(c.point, c.time, created, no_change); !r) {
                return std::unexpected(r.error());
            }
            t_batch = std::max(t_batch, c.time);
            // An event that only replaced its vertices by identical ones (a
            // split whose edge was not there after all, rings touching at a
            // vertex) must not be found again: later split searches for
            // those vertices start after it.
            if (no_change) {
                split_floor.resize(vertices_.size(), -1.0);
                for (std::size_t i = before; i < created.size(); ++i) {
                    split_floor[created[i]] = c.time + tol_.time;
                }
            }
        }
        now_ = t_batch;
        if (auto r = close_degenerate_loops(created, now_); !r) {
            return std::unexpected(r.error());
        }
        refresh_events(created);
        split_floor.resize(vertices_.size(), -1.0);
        for (const std::uint32_t v : created) {
            WaveVertex& w = vertices_[v];
            if (w.alive && w.split_time <= split_floor[v]) {
                // Look for the next split strictly after the rejected one.
                const double saved = now_;
                now_ = split_floor[v];
                compute_split_event(v);
                now_ = saved;
            }
        }
    }
    return {};
}

} // namespace engine::geometry::skeleton_detail
