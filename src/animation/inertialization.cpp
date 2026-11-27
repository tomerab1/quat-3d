// Inertialization: offset capture + Bollo's quintic decay (GDC 2018).

#include "engine/animation/inertialization.hpp"

#include <algorithm>
#include <cmath>

namespace engine::animation {

namespace {

constexpr float k_min_offset = 1e-6F;

// Angular velocity (parent frame) taking `from` to `to` over `dt`.
[[nodiscard]] glm::vec3 angular_velocity(const glm::quat& from, const glm::quat& to, float dt) {
    if (!(dt > 0.0F)) return glm::vec3(0.0F);
    glm::quat d = to * glm::inverse(from);
    if (d.w < 0.0F) d = -d;
    const glm::vec3 v(d.x, d.y, d.z);
    const float s = glm::length(v);
    if (s < 1e-9F) return glm::vec3(0.0F);
    const float angle = 2.0F * std::atan2(s, d.w);
    return v / s * (angle / dt);
}

[[nodiscard]] glm::vec3 linear_velocity(const glm::vec3& from, const glm::vec3& to, float dt) {
    return dt > 0.0F ? (to - from) / dt : glm::vec3(0.0F);
}

// Offset `offset` (with rate `rate`) as a unit direction + scalar curve.
void vector_curve(const glm::vec3& offset, const glm::vec3& rate, float blend_time,
                  glm::vec3& dir, InertialCurve& curve) {
    const float len = glm::length(offset);
    if (len < k_min_offset) {
        dir = glm::vec3(0.0F);
        curve = InertialCurve{};
        return;
    }
    dir = offset / len;
    curve = InertialCurve::make(len, glm::dot(rate, dir), blend_time);
}

} // namespace

InertialCurve InertialCurve::make(float x0, float v0, float blend_time) {
    InertialCurve c;
    if (!(blend_time > 0.0F) || !(x0 > 0.0F) || !std::isfinite(x0) || !std::isfinite(v0)) {
        return c;
    }
    float t1 = blend_time;
    if (v0 < 0.0F) t1 = std::min(t1, -5.0F * x0 / v0); // no overshoot past zero
    t1 = std::max(t1, 1e-5F);
    const float t2 = t1 * t1;
    const float t3 = t2 * t1;
    const float a0 = std::max((-8.0F * v0 * t1 - 20.0F * x0) / t2, 0.0F);
    c.x0 = x0;
    c.v0 = v0;
    c.t1 = t1;
    c.a = -(a0 * t2 + 6.0F * v0 * t1 + 12.0F * x0) / (2.0F * t3 * t2);
    c.b = (3.0F * a0 * t2 + 16.0F * v0 * t1 + 30.0F * x0) / (2.0F * t2 * t2);
    c.c = -(3.0F * a0 * t2 + 12.0F * v0 * t1 + 20.0F * x0) / (2.0F * t3);
    c.d = 0.5F * a0;
    return c;
}

float InertialCurve::evaluate(float t) const {
    if (!(t1 > 0.0F) || t >= t1) return 0.0F;
    if (t <= 0.0F) return x0;
    return ((((a * t + b) * t + c) * t + d) * t + v0) * t + x0;
}

void Inertializer::begin(std::span<const JointPose> source, std::span<const JointPose> source_prev,
                         float source_dt, std::span<const JointPose> target,
                         std::span<const JointPose> target_next, float target_dt,
                         float blend_time) {
    const std::size_t n = std::min({source.size(), source_prev.size(), target.size(),
                                    target_next.size()});
    joints.assign(n, JointInertialization{});
    elapsed = 0.0F;
    duration = std::max(blend_time, 0.0F);

    for (std::size_t i = 0; i < n; ++i) {
        const JointPose& s = source[i];
        const JointPose& t = target[i];
        JointInertialization& j = joints[i];

        vector_curve(s.translation - t.translation,
                     linear_velocity(source_prev[i].translation, s.translation, source_dt) -
                         linear_velocity(t.translation, target_next[i].translation, target_dt),
                     duration, j.translation_dir, j.translation);
        vector_curve(s.scale - t.scale,
                     linear_velocity(source_prev[i].scale, s.scale, source_dt) -
                         linear_velocity(t.scale, target_next[i].scale, target_dt),
                     duration, j.scale_dir, j.scale);

        // Rotation offset q = source * target^-1 (applied in the parent frame);
        // its rate is w_source - q * w_target.
        glm::quat q = glm::normalize(s.rotation) * glm::inverse(glm::normalize(t.rotation));
        if (q.w < 0.0F) q = -q;
        const glm::vec3 v(q.x, q.y, q.z);
        const float sin_half = glm::length(v);
        if (sin_half < k_min_offset) {
            j.rotation = InertialCurve{};
            continue;
        }
        j.rotation_axis = v / sin_half;
        const float angle = 2.0F * std::atan2(sin_half, q.w);
        const glm::vec3 w_offset =
            angular_velocity(source_prev[i].rotation, s.rotation, source_dt) -
            q * angular_velocity(t.rotation, target_next[i].rotation, target_dt);
        j.rotation = InertialCurve::make(angle, glm::dot(w_offset, j.rotation_axis), duration);
    }
}

void Inertializer::apply(std::span<JointPose> pose) const {
    if (!active()) return;
    const std::size_t n = std::min(pose.size(), joints.size());
    for (std::size_t i = 0; i < n; ++i) {
        const JointInertialization& j = joints[i];
        JointPose& p = pose[i];
        p.translation += j.translation_dir * j.translation.evaluate(elapsed);
        p.scale += j.scale_dir * j.scale.evaluate(elapsed);
        const float angle = j.rotation.evaluate(elapsed);
        if (angle != 0.0F) {
            p.rotation = glm::normalize(glm::angleAxis(angle, j.rotation_axis) * p.rotation);
        }
    }
}

} // namespace engine::animation
