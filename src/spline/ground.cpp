// Ground projection of curves (ROADMAP G18.1).

#include "engine/spline/ground.hpp"

#include <cmath>
#include <vector>

namespace engine::spline {

std::size_t drape(std::span<CurveSample> samples, const GroundHeightFn& ground, float offset) {
    if (!ground) return samples.size();
    std::size_t missing = 0;
    std::vector<char> hit(samples.size(), 0);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CurveSample& s = samples[i];
        const std::optional<float> y = ground(s.position.x, s.position.z);
        if (y && std::isfinite(*y)) {
            s.position.y = *y + offset;
            hit[i] = 1;
        } else {
            ++missing;
        }
    }
    // Slope from the neighbours (central differences in horizontal distance).
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (hit[i] == 0) continue;
        const std::size_t a = i > 0 && hit[i - 1] != 0 ? i - 1 : i;
        const std::size_t b = i + 1 < samples.size() && hit[i + 1] != 0 ? i + 1 : i;
        if (a == b) continue;
        const glm::vec3 d = samples[b].position - samples[a].position;
        const float run = std::sqrt(d.x * d.x + d.z * d.z);
        glm::vec3& t = samples[i].tangent;
        const float h = std::sqrt(t.x * t.x + t.z * t.z);
        if (!(run > 1.0e-6F) || !(h > 1.0e-6F)) continue;
        const glm::vec3 dir(t.x / h, d.y / run, t.z / h);
        t = dir / std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    }
    return missing;
}

std::size_t project_points(Spline& spline, const GroundHeightFn& ground, float offset) {
    if (!ground) return spline.points.size();
    std::size_t missing = 0;
    for (SplinePoint& p : spline.points) {
        const std::optional<float> y = ground(p.position.x, p.position.z);
        if (y && std::isfinite(*y)) {
            p.position.y = *y + offset;
        } else {
            ++missing;
        }
    }
    return missing;
}

} // namespace engine::spline
