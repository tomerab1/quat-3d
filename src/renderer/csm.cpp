#include "engine/renderer/csm.hpp"

#include <algorithm>
#include <cmath>

namespace engine::renderer {

namespace {

// One corner ray of the view frustum: its points at NDC depth 0 and 1 in
// view space, and their view distances.
struct CornerRay {
    glm::vec3 near_p{0.0F};
    glm::vec3 far_p{0.0F};
    float     near_d = 0.0F;
    float     far_d = 1.0F;
};

struct LightBasis {
    glm::vec3 side{1.0F, 0.0F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, -1.0F}; // direction the light travels
};

struct Slice {
    glm::mat4 view_proj{1.0F};
    float     texel = 0.0F;
    float     depth_range = 0.0F;
};

[[nodiscard]] glm::vec3 unproject(const glm::mat4& inv_proj, float x, float y, float z) {
    const glm::vec4 p = inv_proj * glm::vec4(x, y, z, 1.0F);
    return glm::vec3(p) / p.w;
}

[[nodiscard]] std::array<CornerRay, 4> corner_rays(const glm::mat4& projection) {
    const glm::mat4 inv_proj = glm::inverse(projection);
    const std::array<glm::vec2, 4> ndc{{{-1.0F, -1.0F}, {1.0F, -1.0F}, {-1.0F, 1.0F}, {1.0F, 1.0F}}};
    std::array<CornerRay, 4> rays{};
    for (std::size_t i = 0; i < ndc.size(); ++i) {
        rays[i].near_p = unproject(inv_proj, ndc[i].x, ndc[i].y, 0.0F);
        rays[i].far_p = unproject(inv_proj, ndc[i].x, ndc[i].y, 1.0F);
        rays[i].near_d = -rays[i].near_p.z;
        rays[i].far_d = -rays[i].far_p.z;
    }
    return rays;
}

// The light's basis depends only on its direction (never on the camera),
// which is what keeps texel snapping meaningful.
[[nodiscard]] LightBasis light_basis(const glm::vec3& to_light) {
    LightBasis b;
    b.forward = -glm::normalize(to_light);
    const glm::vec3 world_up =
        std::abs(b.forward.y) > 0.99F ? glm::vec3(0.0F, 0.0F, 1.0F) : glm::vec3(0.0F, 1.0F, 0.0F);
    b.side = glm::normalize(glm::cross(b.forward, world_up));
    b.up = glm::cross(b.side, b.forward);
    return b;
}

// `v` on a grid of 2^(floor(log2 v) - bits): exact power-of-two steps, so the
// GPU port (exponent bits) lands on the same values.
[[nodiscard]] float octave_step(float v, int bits) {
    int e = 0;
    (void)std::frexp(v, &e); // v = m * 2^e, m in [0.5, 1): floor(log2 v) = e - 1
    return std::ldexp(1.0F, e - 1 - bits);
}

[[nodiscard]] float quantize(float v, int bits, bool up) {
    if (!(v > 0.0F)) return 0.0F;
    const float step = octave_step(v, bits);
    const float units = v / step;
    return (up ? std::ceil(units) : std::floor(units)) * step;
}

constexpr int kRangeBits = 3;  // depth range: 1/8-octave steps
constexpr int kRadiusBits = 4; // cascade radius: 1/16-octave steps

[[nodiscard]] Slice fit_slice(const glm::mat4& inv_view, const std::array<CornerRay, 4>& rays,
                              const LightBasis& light, float start, float end,
                              std::uint32_t map_size, float caster_distance) {
    std::array<glm::vec3, 8> corners{};
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const CornerRay& r = rays[i];
        const float span = r.far_d - r.near_d;
        const float t0 = (start - r.near_d) / span;
        const float t1 = (end - r.near_d) / span;
        corners[i] = glm::vec3(inv_view * glm::vec4(glm::mix(r.near_p, r.far_p, t0), 1.0F));
        corners[i + 4] = glm::vec3(inv_view * glm::vec4(glm::mix(r.near_p, r.far_p, t1), 1.0F));
    }
    glm::vec3 centre(0.0F);
    for (const glm::vec3& c : corners) centre += c;
    centre *= 0.125F;
    float radius = 0.0F;
    for (const glm::vec3& c : corners) radius = std::max(radius, glm::length(c - centre));
    // Snapping moves the centre by up to one texel; growing the radius by a
    // texel's worth first keeps the whole slice inside the snapped square.
    const float map = static_cast<float>(map_size);
    radius = quantize(radius * (map / (map - 2.0F)), kRadiusBits, /*up=*/true);

    const float texel = 2.0F * radius / map;
    const float lx = glm::dot(light.side, centre);
    const float ly = glm::dot(light.up, centre);
    const float lz = glm::dot(light.forward, centre); // distance along the light's travel
    const float cx = std::floor(lx / texel) * texel;
    const float cy = std::floor(ly / texel) * texel;
    const float near_d = lz - radius - caster_distance;
    const float far_d = lz + radius;
    const float depth_span = far_d - near_d;

    // Rows of ortho(cx +- r, cy +- r, near_d, far_d) * lookAt(0, forward, up),
    // written out (glm::mat4 is column-major: m[col][row]).
    Slice s;
    const float inv_r = 1.0F / radius;
    const float inv_d = 1.0F / depth_span;
    const glm::vec4 row0(light.side * inv_r, -cx * inv_r);
    const glm::vec4 row1(light.up * inv_r, -cy * inv_r);
    const glm::vec4 row2(light.forward * inv_d, -near_d * inv_d);
    const glm::vec4 row3(0.0F, 0.0F, 0.0F, 1.0F);
    s.view_proj = glm::transpose(glm::mat4(row0, row1, row2, row3));
    s.texel = texel;
    s.depth_range = depth_span;
    return s;
}

} // namespace

float view_distance_at_depth(const glm::mat4& projection, float depth) {
    const glm::vec3 p = unproject(glm::inverse(projection), 0.0F, 0.0F, depth);
    return -p.z;
}

std::array<float, max_cascades + 1> cascade_splits(float near_dist, float far_dist,
                                                   std::uint32_t count, float lambda) {
    count = std::clamp(count, 1U, max_cascades);
    std::array<float, max_cascades + 1> splits{};
    splits.fill(far_dist);
    splits[0] = near_dist;
    const float ratio = far_dist / near_dist;
    for (std::uint32_t i = 1; i < count; ++i) {
        const float k = static_cast<float>(i) / static_cast<float>(count);
        const float log_split = near_dist * std::pow(ratio, k);
        const float lin_split = near_dist + (far_dist - near_dist) * k;
        splits[i] = lambda * log_split + (1.0F - lambda) * lin_split;
    }
    splits[count] = far_dist;
    return splits;
}

float quantize_down(float v) { return quantize(v, kRangeBits, /*up=*/false); }
float quantize_up(float v) { return quantize(v, kRangeBits, /*up=*/true); }

glm::vec2 shadow_range(const glm::mat4& projection, const CsmSettings& settings,
                       bool bounds_valid, float depth_min, float depth_max) {
    const CsmSettings s = settings.sanitized();
    const float cam_near = view_distance_at_depth(projection, 0.0F);
    const float cam_far = view_distance_at_depth(projection, 1.0F);
    const float full_far = std::min(s.max_distance, cam_far);
    float lo = cam_near;
    float hi = full_far;
    if (s.gpu_depth_bounds && bounds_valid && depth_min <= depth_max) {
        lo = std::max(cam_near, quantize_down(view_distance_at_depth(projection, depth_min)));
        hi = std::min(full_far, quantize_up(view_distance_at_depth(projection, depth_max)));
        if (!(hi > lo)) {
            lo = cam_near;
            hi = full_far;
        }
    }
    return {lo, hi};
}

GpuCascades fit_cascades(const CsmView& view, const CsmSettings& settings, glm::vec2 range,
                         std::uint32_t map_size) {
    const CsmSettings s = settings.sanitized();
    const std::array<CornerRay, 4> rays = corner_rays(view.projection);
    const glm::mat4 inv_view = glm::inverse(view.view);
    const LightBasis light = light_basis(view.to_light);
    const auto splits = cascade_splits(range.x, range.y, s.cascade_count, s.split_lambda);

    GpuCascades out;
    for (std::uint32_t i = 0; i < max_cascades; ++i) {
        const std::uint32_t c = std::min(i, s.cascade_count - 1);
        float start = splits[c];
        if (c > 0) start -= s.blend_band * (splits[c] - splits[c - 1]);
        const Slice slice = fit_slice(inv_view, rays, light, start, splits[c + 1], map_size,
                                      s.caster_distance);
        out.view_proj[i] = slice.view_proj;
        out.split_far[static_cast<int>(i)] = splits[c + 1];
        out.texel_world[static_cast<int>(i)] = slice.texel;
        out.depth_range[static_cast<int>(i)] = slice.depth_range;
    }
    out.params = glm::vec4(static_cast<float>(s.cascade_count), range.x, s.blend_band,
                           s.gpu_depth_bounds ? 1.0F : 0.0F);
    return out;
}

glm::mat4 caster_cull_view_proj(const CsmView& view, const CsmSettings& settings,
                                std::uint32_t map_size) {
    const CsmSettings s = settings.sanitized();
    const float cam_near = view_distance_at_depth(view.projection, 0.0F);
    const float cam_far = view_distance_at_depth(view.projection, 1.0F);
    return fit_slice(glm::inverse(view.view), corner_rays(view.projection),
                     light_basis(view.to_light), cam_near, std::min(s.max_distance, cam_far),
                     map_size, s.caster_distance)
        .view_proj;
}

} // namespace engine::renderer
