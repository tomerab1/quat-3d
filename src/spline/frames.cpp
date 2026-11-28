// Curve frames: up-locked and rotation-minimising (ROADMAP G18.1).
//
// Rotation-minimising frames by double reflection: W. Wang, B. Jüttler,
// D. Zheng, Y. Liu, "Computation of Rotation Minimizing Frames", ACM TOG
// 27(1), 2008, Algorithm 1. Each step reflects the frame in the bisector
// plane of the chord x[i] -> x[i+1], then in the plane that maps the
// reflected tangent onto t[i+1]; the composition approximates the RMF to
// fourth order, uses no trigonometry and never flips.

#include "engine/spline/frames.hpp"

#include <cmath>
#include <string_view>

namespace engine::spline {

namespace {

[[nodiscard]] float dot3(const glm::vec3& a, const glm::vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

[[nodiscard]] glm::vec3 cross3(const glm::vec3& a, const glm::vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] bool normalize_into(const glm::vec3& v, glm::vec3& out) {
    const float len2 = dot3(v, v);
    if (!(len2 > 1.0e-20F)) return false;
    out = v / std::sqrt(len2);
    return true;
}

// The unit vector perpendicular to `t` closest to `up` (Gram–Schmidt); when
// t is parallel to up, a fixed perpendicular instead.
[[nodiscard]] glm::vec3 perpendicular_up(const glm::vec3& t, const glm::vec3& up) {
    glm::vec3 out;
    if (normalize_into(up - t * dot3(up, t), out)) return out;
    const glm::vec3 alt = std::abs(t.x) < 0.9F ? glm::vec3(1.0F, 0.0F, 0.0F) : glm::vec3(0.0F, 0.0F, 1.0F);
    if (normalize_into(alt - t * dot3(alt, t), out)) return out;
    return glm::vec3(0.0F, 1.0F, 0.0F);
}

[[nodiscard]] Frame base_frame(const CurveSample& s) {
    Frame f;
    f.position = s.position;
    f.tangent = s.tangent;
    f.width = s.width;
    f.height = s.height;
    f.tags = s.tags;
    f.param = s.param;
    f.distance = s.distance;
    return f;
}

} // namespace

glm::mat4 Frame::matrix() const {
    return {glm::vec4(tangent, 0.0F), glm::vec4(up, 0.0F), glm::vec4(right, 0.0F), glm::vec4(position, 1.0F)};
}

Frame up_locked_frame(const CurveSample& s, const glm::vec3& world_up) {
    Frame f = base_frame(s);
    glm::vec3 up;
    if (!normalize_into(world_up, up)) up = glm::vec3(0.0F, 1.0F, 0.0F);
    // Tangent with its component along up removed: horizontal for +Y.
    glm::vec3 flat;
    if (!normalize_into(s.tangent - up * dot3(s.tangent, up), flat)) {
        // A vertical tangent has no heading: any horizontal direction will do;
        // take the one perpendicular to up closest to +X (deterministic).
        flat = perpendicular_up(up, glm::vec3(1.0F, 0.0F, 0.0F));
    }
    f.tangent = flat;
    f.up = up;
    f.right = cross3(flat, up);
    return f;
}

std::vector<Frame> frames(std::span<const CurveSample> samples, FrameMode mode, const glm::vec3& world_up) {
    std::vector<Frame> out;
    out.reserve(samples.size());
    if (samples.empty()) return out;
    if (mode == FrameMode::up_locked) {
        for (const CurveSample& s : samples) out.push_back(up_locked_frame(s, world_up));
        return out;
    }
    glm::vec3 up_dir;
    if (!normalize_into(world_up, up_dir)) up_dir = glm::vec3(0.0F, 1.0F, 0.0F);
    Frame f = base_frame(samples[0]);
    f.up = perpendicular_up(f.tangent, up_dir);
    f.right = cross3(f.tangent, f.up);
    out.push_back(f);
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const Frame& prev = out.back();
        Frame next = base_frame(samples[i]);
        const glm::vec3 v1 = next.position - prev.position;
        const float c1 = dot3(v1, v1);
        glm::vec3 r_l = prev.up;
        glm::vec3 t_l = prev.tangent;
        if (c1 > 1.0e-20F) {
            r_l = prev.up - v1 * (2.0F / c1 * dot3(v1, prev.up));
            t_l = prev.tangent - v1 * (2.0F / c1 * dot3(v1, prev.tangent));
        }
        const glm::vec3 v2 = next.tangent - t_l;
        const float c2 = dot3(v2, v2);
        glm::vec3 up = c2 > 1.0e-20F ? r_l - v2 * (2.0F / c2 * dot3(v2, r_l)) : r_l;
        // Re-orthonormalise against float drift over long curves.
        up = perpendicular_up(next.tangent, up);
        next.up = up;
        next.right = cross3(next.tangent, up);
        out.push_back(next);
    }
    return out;
}

std::vector<Frame> sample_frames(const Curve& curve, const SpacingOptions& spacing, FrameMode mode,
                                 const glm::vec3& world_up) {
    const std::vector<CurveSample> samples = sample_by_spacing(curve, spacing);
    return frames(samples, mode, world_up);
}

std::string_view frame_mode_name(FrameMode mode) {
    return mode == FrameMode::up_locked ? "up_locked" : "rotation_minimising";
}

bool parse_frame_mode(std::string_view name, FrameMode& out) {
    if (name == "up_locked") {
        out = FrameMode::up_locked;
        return true;
    }
    if (name == "rotation_minimising" || name == "rmf") {
        out = FrameMode::rotation_minimising;
        return true;
    }
    return false;
}

} // namespace engine::spline
