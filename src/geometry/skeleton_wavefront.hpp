#pragma once

// Wavefront propagation for the weighted straight skeleton (private to
// engine_geometry; the public API is engine/geometry/straight_skeleton.hpp,
// which cites the algorithms).
//
// Every input edge is a moving line: points p with dot(normal, p - point) =
// weight * t at time t (normal points into the polygon). A wavefront vertex
// sits on two such lines, so its velocity is fixed until an event replaces
// it. Events are found per vertex and cached:
//   - edge event: the vertex's outgoing wavefront edge shrinks to a point;
//   - split event: a reflex vertex reaches a non-incident wavefront edge.
// All events within the time / distance tolerances of each other are then
// resolved together, per point, as one generalised intersection event.

#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/straight_skeleton.hpp"

namespace engine::geometry::skeleton_detail {

inline constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
inline constexpr double kNever = std::numeric_limits<double>::infinity();

struct MovingLine {
    glm::dvec2 point{0.0};  // a point on the line at t = 0
    glm::dvec2 dir{1.0, 0.0};
    glm::dvec2 normal{0.0, 1.0}; // inward: dir rotated +90 degrees
    double weight = 1.0;
};

struct WaveVertex {
    glm::dvec2 origin{0.0}; // position at time t0
    double t0 = 0.0;
    glm::dvec2 velocity{0.0};
    std::uint32_t left_line = 0;  // line of the edge arriving at this vertex
    std::uint32_t right_line = 0; // line of the edge leaving it
    std::uint32_t prev = kNone;
    std::uint32_t next = kNone;
    std::uint32_t node = kNone; // skeleton node where this vertex was born
    bool alive = true;
    bool reflex = false;
    // Cached events.
    double edge_time = kNever;   // collapse of the edge (this -> next)
    double split_time = kNever;
    std::uint32_t split_edge = kNone;      // start vertex of the edge it hits
    std::uint32_t split_edge_next = kNone; // that edge's end when computed

    [[nodiscard]] glm::dvec2 at(double t) const { return origin + velocity * (t - t0); }
};

struct Tolerances {
    double distance = 0.0; // points closer than this coincide
    double time = 0.0;     // events closer than this are simultaneous
};

class Wavefront {
public:
    // `rings` are oriented (outer CCW, holes CW), free of duplicate and
    // collinear-equal-weight vertices; `weights` has one entry per edge in
    // ring order. Input vertices become nodes 0..n-1 of `out`.
    [[nodiscard]] std::expected<void, core::Error> run(const std::vector<std::vector<glm::dvec2>>& rings,
                                                       const std::vector<double>& weights,
                                                       StraightSkeleton& out);

private:
    struct Candidate {
        double time = 0.0;
        glm::dvec2 point{0.0};
        std::uint32_t vertex = 0;
    };
    struct Chain {
        std::uint32_t in_line = 0;
        std::uint32_t out_line = 0;
        std::uint32_t in_far = kNone;  // vertex at the far end of the incoming edge
        std::uint32_t out_far = kNone; // vertex at the far end of the outgoing edge
    };

    [[nodiscard]] std::expected<glm::dvec2, core::Error> velocity(std::uint32_t left, std::uint32_t right,
                                                                  bool& ridge) const;
    [[nodiscard]] std::expected<std::uint32_t, core::Error> make_vertex(glm::dvec2 position, double t,
                                                                        std::uint32_t left, std::uint32_t right,
                                                                        std::uint32_t node);
    void compute_edge_event(std::uint32_t v);
    void compute_split_event(std::uint32_t v);
    [[nodiscard]] double split_time_against(std::uint32_t v, std::uint32_t edge_start) const;
    [[nodiscard]] bool edge_is_current(std::uint32_t v) const;
    std::uint32_t add_node(glm::dvec2 position, double t);
    void kill(std::uint32_t v, std::uint32_t node);
    void add_arc(std::uint32_t from, std::uint32_t to, std::uint32_t left, std::uint32_t right);
    void add_ridge(std::uint32_t from, std::uint32_t to, std::uint32_t line_a, std::uint32_t line_b);
    [[nodiscard]] std::expected<void, core::Error> resolve(glm::dvec2 x, double t,
                                                           std::vector<std::uint32_t>& created, bool& no_change);
    [[nodiscard]] std::expected<void, core::Error> close_degenerate_loops(std::vector<std::uint32_t>& created,
                                                                          double t);
    void refresh_events(const std::vector<std::uint32_t>& created);

    std::vector<MovingLine> lines_;
    std::vector<WaveVertex> vertices_;
    StraightSkeleton* out_ = nullptr;
    Tolerances tol_{};
    double now_ = 0.0;
};

} // namespace engine::geometry::skeleton_detail
