#include "engine/renderer/light_clusters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// NDC depth used to unproject a tile corner into its view ray. Any depth
// strictly inside (0, 1) lies on the ray at a finite distance for standard,
// reversed and infinite projections alike; 0.5 avoids the plane at infinity.
constexpr float kRayNdcDepth = 0.5F;

} // namespace

GpuLight make_point_light(const glm::vec3& position, float range, const glm::vec3& color,
                          float intensity) {
    GpuLight l;
    l.position_range = glm::vec4(position, range);
    l.color_intensity = glm::vec4(color, intensity);
    l.type = static_cast<std::uint32_t>(LightType::point);
    return l;
}

GpuLight make_spot_light(const glm::vec3& position, const glm::vec3& direction, float range,
                         float inner_cone, float outer_cone, const glm::vec3& color,
                         float intensity) {
    GpuLight l = make_point_light(position, range, color, intensity);
    l.type = static_cast<std::uint32_t>(LightType::spot);
    l.direction = glm::normalize(direction);
    const float cos_outer = std::cos(outer_cone);
    const float cos_inner = std::max(std::cos(inner_cone), cos_outer);
    const float scale = 1.0F / std::max(cos_inner - cos_outer, 1e-4F);
    l.spot_scale_offset = glm::vec4(scale, -cos_outer * scale, 0.0F, 0.0F);
    return l;
}

std::expected<ClusterParams, core::Error>
make_cluster_params(const glm::mat4& view, const glm::mat4& proj, std::uint32_t light_count) {
    // A right-handed perspective projection writes -z_view into clip w:
    // column 2 row 3 is -1 and column 3 row 3 is 0 (TAA jitter and the
    // Vulkan Y flip leave both untouched).
    if (std::abs(proj[2][3] + 1.0F) > 1e-4F || std::abs(proj[3][3]) > 1e-4F) {
        return fail("light clusters: projection is not a right-handed perspective");
    }
    // [0,1]-depth perspective: z_ndc = (P22·z + P32) / -z. Standard Z gives
    // P32/P22 = near and P32/(P22+1) = far; reversed Z swaps the two.
    const float p22 = proj[2][2];
    const float p32 = proj[3][2];
    const float a = p32 / p22;
    const float b = p32 / (p22 + 1.0F);
    const float near_z = std::min(a, b);
    const float far_z = std::max(a, b);
    if (!std::isfinite(near_z) || !std::isfinite(far_z) || near_z <= 0.0F || far_z <= near_z) {
        return fail("light clusters: projection has no finite near/far range");
    }

    ClusterParams p;
    p.view = view;
    p.proj = proj;
    p.inv_proj = glm::inverse(proj);
    p.dims = glm::uvec4(cluster_grid_x, cluster_grid_y, cluster_grid_z,
                        std::min(light_count, max_lights));
    const float scale = static_cast<float>(cluster_grid_z) / std::log(far_z / near_z);
    p.depth = glm::vec4(near_z, far_z, scale, std::log(near_z) * scale);
    p.limits = glm::uvec4(max_cluster_light_indices, 0U, 0U, 0U);
    return p;
}

std::uint32_t depth_slice(const ClusterParams& params, float view_depth) {
    if (!(view_depth > 0.0F)) return 0;
    const float s = std::floor(std::log(view_depth) * params.depth.z - params.depth.w);
    const float last = static_cast<float>(params.dims.z - 1U);
    return static_cast<std::uint32_t>(std::clamp(s, 0.0F, last));
}

float slice_boundary(const ClusterParams& params, std::uint32_t k) {
    const float t = static_cast<float>(k) / static_cast<float>(params.dims.z);
    return params.depth.x * std::pow(params.depth.y / params.depth.x, t);
}

glm::uvec3 cluster_of_view_position(const ClusterParams& params, const glm::vec3& view_pos) {
    const glm::vec4 clip = params.proj * glm::vec4(view_pos, 1.0F);
    const float w = std::max(clip.w, 1e-6F);
    const glm::vec2 uv = glm::vec2(clip.x, clip.y) / w * 0.5F + 0.5F;
    const auto tile = [](float u, std::uint32_t n) {
        const float t = std::floor(u * static_cast<float>(n));
        return static_cast<std::uint32_t>(std::clamp(t, 0.0F, static_cast<float>(n - 1U)));
    };
    return {tile(uv.x, params.dims.x), tile(uv.y, params.dims.y),
            depth_slice(params, -view_pos.z)};
}

ClusterAabb cluster_aabb(const ClusterParams& params, const glm::uvec3& c) {
    const glm::vec2 dims(static_cast<float>(params.dims.x), static_cast<float>(params.dims.y));
    const glm::vec2 ndc_min = glm::vec2(c.x, c.y) / dims * 2.0F - 1.0F;
    const glm::vec2 ndc_max = glm::vec2(c.x + 1U, c.y + 1U) / dims * 2.0F - 1.0F;
    const float z_near = slice_boundary(params, c.z);
    const float z_far = slice_boundary(params, c.z + 1U);

    constexpr float inf = std::numeric_limits<float>::infinity();
    ClusterAabb box{glm::vec3(inf), glm::vec3(-inf)};
    const std::array<glm::vec2, 4> corners{
        {{ndc_min.x, ndc_min.y}, {ndc_max.x, ndc_min.y}, {ndc_min.x, ndc_max.y},
         {ndc_max.x, ndc_max.y}}};
    for (const glm::vec2& ndc : corners) {
        const glm::vec4 h = params.inv_proj * glm::vec4(ndc, kRayNdcDepth, 1.0F);
        const glm::vec3 p = glm::vec3(h) / h.w;
        const glm::vec3 ray = p / -p.z; // the corner ray, scaled to view z = -1
        for (const float d : {z_near, z_far}) {
            box.min = glm::min(box.min, ray * d);
            box.max = glm::max(box.max, ray * d);
        }
    }
    return box;
}

float aabb_distance_sq(const ClusterAabb& box, const glm::vec3& p) {
    const glm::vec3 d = glm::max(glm::max(box.min - p, p - box.max), glm::vec3(0.0F));
    return glm::dot(d, d);
}

bool sphere_overlaps_aabb(const ClusterAabb& box, const glm::vec3& centre, float radius) {
    return radius > 0.0F && aabb_distance_sq(box, centre) <= radius * radius;
}

std::vector<std::vector<std::uint32_t>>
assign_lights_to_clusters(const ClusterParams& params, std::span<const GpuLight> lights) {
    std::vector<glm::vec3> centres;
    centres.reserve(lights.size());
    for (const GpuLight& l : lights) {
        centres.emplace_back(params.view * glm::vec4(glm::vec3(l.position_range), 1.0F));
    }

    std::vector<std::vector<std::uint32_t>> out(cluster_count);
    for (std::uint32_t z = 0; z < cluster_grid_z; ++z) {
        for (std::uint32_t y = 0; y < cluster_grid_y; ++y) {
            for (std::uint32_t x = 0; x < cluster_grid_x; ++x) {
                const glm::uvec3 c(x, y, z);
                const ClusterAabb box = cluster_aabb(params, c);
                std::vector<std::uint32_t>& list = out[cluster_index(c)];
                for (std::uint32_t i = 0; i < lights.size(); ++i) {
                    if (sphere_overlaps_aabb(box, centres[i], lights[i].position_range.w)) {
                        list.push_back(i);
                    }
                }
            }
        }
    }
    return out;
}

} // namespace engine::renderer
