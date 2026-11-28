// CPU reference of the per-view instance cull — see
// include/engine/renderer/instance_cull.hpp. instance_cull.slang mirrors
// these functions operation for operation.

#include "engine/renderer/instance_cull.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::renderer {

std::array<glm::vec4, 6> frustum_planes(const glm::mat4& view_proj) {
    // Gribb & Hartmann 2001: the rows of the clip matrix give the planes;
    // Vulkan's [0, 1] depth makes the near plane row 2 alone.
    const glm::mat4 m = glm::transpose(view_proj);
    std::array<glm::vec4, 6> planes{m[3] + m[0], m[3] - m[0], m[3] + m[1],
                                    m[3] - m[1], m[2],        m[3] - m[2]};
    for (glm::vec4& p : planes) {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0F) p /= len;
    }
    return planes;
}

CullViewGpu camera_cull_view(const glm::mat4& view_proj) {
    CullViewGpu v;
    v.planes = frustum_planes(view_proj);
    v.kind = static_cast<std::uint32_t>(CullViewKind::planes);
    return v;
}

CullViewGpu cascade_cull_view(std::uint32_t cascade, float min_caster_texels) {
    CullViewGpu v;
    v.kind = static_cast<std::uint32_t>(CullViewKind::cascade);
    v.cascade = cascade;
    v.min_caster_texels = std::max(min_caster_texels, 0.0F);
    return v;
}

CullViewGpu keep_all_cull_view() {
    CullViewGpu v;
    v.kind = static_cast<std::uint32_t>(CullViewKind::all);
    return v;
}

ResolvedCullView resolve_cull_view(const CullViewGpu& view, const GpuCascades& cascades) {
    ResolvedCullView out;
    switch (static_cast<CullViewKind>(view.kind)) {
    case CullViewKind::planes:
        out.planes = view.planes;
        break;
    case CullViewKind::cascade: {
        const std::uint32_t c = std::min(view.cascade, max_cascades - 1);
        out.planes = frustum_planes(cascades.view_proj[c]);
        out.near_extrude = std::max(cascades.limits.y, 0.0F);
        out.texel_world = cascades.texel_world[static_cast<glm::length_t>(c)];
        out.min_caster_texels = view.min_caster_texels;
        out.shadow = true;
        break;
    }
    default:
        out.keep_all = true;
        break;
    }
    return out;
}

glm::vec4 world_sphere(const glm::mat4& model, const glm::vec4& local) {
    const glm::vec3 centre(model * glm::vec4(glm::vec3(local), 1.0F));
    const float s2 = std::max({glm::dot(glm::vec3(model[0]), glm::vec3(model[0])),
                               glm::dot(glm::vec3(model[1]), glm::vec3(model[1])),
                               glm::dot(glm::vec3(model[2]), glm::vec3(model[2]))});
    return {centre, local.w * std::sqrt(s2)};
}

bool cull_accepts(const ResolvedCullView& view, const CullDrawGpu& draw, const glm::vec4& sphere,
                  const glm::vec3& camera) {
    if (view.keep_all || (draw.flags & cull_draw_never_cull) != 0) return true;
    const glm::vec3 c(sphere);
    const float r = sphere.w;
    for (std::size_t i = 0; i < view.planes.size(); ++i) {
        const glm::vec4& p = view.planes[i];
        const float slack = i == 4 ? view.near_extrude : 0.0F;
        if (glm::dot(glm::vec3(p), c) + p.w < -(r + slack)) return false;
    }
    if (view.shadow) {
        // Projected size: a caster narrower than the threshold in texels.
        if (view.texel_world > 0.0F && view.min_caster_texels > 0.0F &&
            2.0F * r < view.min_caster_texels * view.texel_world) {
            return false;
        }
        if (draw.shadow_distance > 0.0F && glm::length(c - camera) - r > draw.shadow_distance) {
            return false;
        }
    }
    return true;
}

std::vector<std::vector<std::vector<std::uint32_t>>>
reference_cull(std::span<const CullViewGpu> views, const GpuCascades& cascades,
               std::span<const CullDrawGpu> draws, std::span<const glm::mat4> models,
               const glm::vec3& camera) {
    std::vector<std::vector<std::vector<std::uint32_t>>> out(views.size());
    for (std::size_t v = 0; v < views.size(); ++v) {
        const ResolvedCullView view = resolve_cull_view(views[v], cascades);
        out[v].resize(draws.size());
        for (std::size_t d = 0; d < draws.size(); ++d) {
            const CullDrawGpu& draw = draws[d];
            for (std::uint32_t k = 0; k < draw.instance_count; ++k) {
                const std::uint32_t i = draw.first_instance + k;
                if (i >= models.size()) break;
                if (cull_accepts(view, draw, world_sphere(models[i], draw.local_sphere), camera)) {
                    out[v][d].push_back(i);
                }
            }
        }
    }
    return out;
}

} // namespace engine::renderer
