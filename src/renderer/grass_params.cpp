#include "grass_params.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "engine/weather/wind_field.hpp"

namespace engine::renderer::grass_detail {

namespace {

// Blades are ~0.5 m tall: the log-profile factor from the field's 10 m
// reference height down to them.
constexpr float kBladeWindHeightM = 0.5F;

} // namespace

glm::vec3 grass_mean_color(const GrassSettings& s) {
    // grass.slang's ramps averaged over the blade's length (smoothstep
    // averages to one half), dry clumps at ~78 % dryness on average.
    const glm::vec3 green = 0.5F * (s.base_color + s.tip_color);
    const glm::vec3 dry_base = glm::mix(s.base_color, s.dry_color, 0.45F);
    const glm::vec3 dry = 0.5F * (dry_base + s.dry_color);
    // A dense field shades its own lower half: what the eye averages is darker.
    constexpr float k_self_shadow = 0.8F;
    return glm::mix(green, dry, s.dry_fraction * 0.78F) * k_self_shadow;
}

GrassParamsGpu make_params(const GrassFrame& frame, const GrassSettings& s, const GrassGrid& grid,
                           const GrassCapacity& capacity, std::span<const TerrainHeightTile> tiles,
                           const ViewMotion& motion, VkExtent2D extent, bool gbuffer_bound) {
    GrassParamsGpu p;
    p.view_proj = frame.draw_view_proj;
    const auto planes = grass_frustum_planes(frame.cull_view_proj);
    std::copy(planes.begin(), planes.end(), p.planes);
    p.camera = glm::vec4(frame.camera, frame.time_s);
    p.grid = glm::ivec4(grid.origin, glm::ivec2(grid.cells));
    p.cell = glm::vec4(grid.cell_size, s.radius, s.lod_start * s.radius,
                       s.lod_start * s.radius);
    p.lod = glm::vec4(s.far_density, s.radius * (1.0F - grass_edge_band),
                      grass_max_blade_height(s), grass_max_width_scale);
    p.blade = glm::vec4(s.height_range, s.width_range);
    const float max_cos = s.slope_deg.y >= 90.0F ? -2.0F : std::cos(glm::radians(s.slope_deg.y));
    const float min_cos = s.slope_deg.x <= 0.0F ? 2.0F : std::cos(glm::radians(s.slope_deg.x));
    p.limits = glm::vec4(max_cos, min_cos, s.height_m);
    p.wind = glm::vec4(s.wind_direction, s.wind_strength, s.wind_frequency);
    p.base_color = glm::vec4(s.base_color, s.terrain_only ? 1.0F : 0.0F);
    p.tip_color = glm::vec4(s.tip_color, s.ground_y);
    p.counts = glm::uvec4(s.seed, static_cast<std::uint32_t>(tiles.size()), capacity.near,
                          capacity.far);
    p.draw = glm::uvec4(near_indices, far_indices, grass_near_segments, grass_far_segments);

    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (std::size_t k = 0; k < tiles.size(); ++k) {
        const TerrainHeightTile& t = tiles[k];
        p.tile_rect[k] = glm::vec4(t.origin_xz, t.size_m, t.base_y);
        p.tile_info[k] = glm::vec4(1.0F / static_cast<float>(t.resolution),
                                   t.size_m / static_cast<float>(t.resolution - 1), 0.0F, 0.0F);
        lo = std::min(lo, t.base_y + t.min_height);
        hi = std::max(hi, t.base_y + t.max_height);
    }
    if (!s.terrain_only) {
        lo = std::min(lo, s.ground_y);
        hi = std::max(hi, s.ground_y);
    }
    p.ground = glm::vec4(lo, hi, grass_slope_fade_start(s), 0.0F);
    p.wind_rect = frame.wind.valid() ? frame.wind.rect : glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);
    p.wind_rect.w = weather::wind_profile(kBladeWindHeightM);
    // Without a field (no weather) the blades keep the R9.3 sway exactly.
    p.wind_flow = frame.wind.valid() ? glm::vec4(frame.wind.heading, 1.0F, 0.0F) : glm::vec4(0.0F);
    p.prev_view_proj = motion.prev_view_proj;
    p.motion = glm::vec4(frame.prev_time_s.value_or(frame.time_s), motion.jitter_ndc, 0.0F);

    p.clump = glm::vec4(s.clump_size, s.clump_strength, grass_max_clump_pull(s), 0.0F);
    p.look = glm::vec4(s.color_variation, s.dry_fraction, s.root_tint,
                       gbuffer_bound ? 1.0F : 0.0F);
    p.dry_color = glm::vec4(s.dry_color, s.min_blade_px);
    p.footprint = glm::vec4(std::max(frame.pixels_per_metre, 0.0F),
                            0.5F * (s.width_range.x + s.width_range.y), s.ground_cover,
                            s.far_ground_cover);
    p.cover_color = glm::vec4(s.ground_cover_color, 0.0F);
    p.far_color = glm::vec4(grass_mean_color(s), 0.0F);
    p.inv_view_proj = glm::inverse(frame.draw_view_proj);
    const glm::vec2 size(static_cast<float>(std::max(extent.width, 1U)),
                         static_cast<float>(std::max(extent.height, 1U)));
    p.screen = glm::vec4(size, 1.0F / size);
    return p;
}

} // namespace engine::renderer::grass_detail
