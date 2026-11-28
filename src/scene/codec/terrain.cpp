// Codec component `terrain`: Procedural terrain tile / streamed world.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/terrain/budget.hpp"
#include "engine/terrain/talus_relax.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<terrain::ErosionModel> k_erosion_models[] = {
    {"droplets", terrain::ErosionModel::droplets}, {"grid", terrain::ErosionModel::grid}};

} // namespace

std::expected<void, core::Error> register_terrain(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "terrain",
                         .summary = "Procedural terrain tile / streamed world",
                         .addable = true,
                         .writable = true,
                         .order = 1700};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* t = &r.get<Terrain>(e);
        out["terrain"] = {{"seed", t->params.seed},
                          {"resolution", t->params.resolution},
                          {"tile_size", t->params.tile_size_m},
                          {"height", t->params.height_m},
                          {"octaves", t->params.octaves},
                          {"frequency", t->params.base_frequency},
                          {"gain", t->params.gain},
                          {"warp", t->params.warp_strength},
                          {"erosion", enum_name(k_erosion_models, t->params.erosion_model)},
                          {"droplets", t->params.erosion_droplets},
                          {"erosion_steps", t->params.grid.iterations},
                          {"erosion_relax", t->params.grid.relax_cycles},
                          {"erosion_talus", t->params.grid.talus},
                          {"erosion_rain", t->params.grid.rain},
                          {"erosion_capacity", t->params.grid.capacity},
                          {"erosion_rate", t->params.grid.erode_rate},
                          {"erosion_thermal", t->params.grid.thermal_rate},
                          {"snowline", t->snowline_m},
                          {"streaming", t->streaming},
                          {"stream_radius", t->stream_radius},
                          {"gpu", t->gpu_generate}};
        return std::move(out["terrain"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "terrain";
        return merge<Terrain>(r, e, v, name, strict, [](JsonFields& f, Terrain& t) {
            terrain::TerrainParams& p = t.params;
            const terrain::TerrainParams before = p;
            f.integer("seed", p.seed, 0, 0xFFFFFFFFLL);
            f.integer("resolution", p.resolution, 17, 8193);
            f.number("tile_size", p.tile_size_m, 1.0F, 100000.0F);
            f.number("height", p.height_m, 0.0F, 100000.0F);
            f.integer("octaves", p.octaves, 1, 16);
            f.number("frequency", p.base_frequency, 0.05F, 64.0F);
            f.number("gain", p.gain, 0.1F, 0.9F);
            f.number("warp", p.warp_strength, 0.0F, 4.0F);
            f.enumeration("erosion", p.erosion_model, k_erosion_models);
            f.integer("droplets", p.erosion_droplets, 0, 50'000'000);
            f.integer("erosion_steps", p.grid.iterations, 0, terrain::max_grid_erosion_steps);
            f.integer("erosion_relax", p.grid.relax_cycles, 0, terrain::max_talus_relax_cycles);
            f.number("erosion_talus", p.grid.talus, 0.05F, 4.0F);
            f.number("erosion_rain", p.grid.rain, 0.0F, 1.0F);
            f.number("erosion_capacity", p.grid.capacity, 0.0F, 20.0F);
            f.number("erosion_rate", p.grid.erode_rate, 0.0F, 10.0F);
            f.number("erosion_thermal", p.grid.thermal_rate, 0.0F, 1.0F);
            f.number("snowline", t.snowline_m);
            f.boolean("streaming", t.streaming);
            f.integer("stream_radius", t.stream_radius, 0, 8);
            f.boolean("gpu", t.gpu_generate);
            // Grid erosion's cost grows with resolution² x steps and has no
            // droplet-style cap on its own: bound it (audit D3).
            if (!terrain::grid_erosion_within_limits(p)) {
                f.fail(f.has("erosion_steps") ? "erosion_steps" : "resolution",
                       "grid erosion is limited to resolution " +
                           std::to_string(terrain::max_grid_erosion_resolution) +
                           " and resolution^2 x erosion_steps <= " +
                           std::to_string(terrain::max_grid_erosion_texel_steps) +
                           " (e.g. 1025 at 5000 steps)");
            }
            // The fields above are bounded one by one; their product (tiles
            // kept loaded x resolution², droplets x tiles) is bounded here (#67).
            if (const auto over = terrain::terrain_budget_problem(p, t.streaming, t.stream_radius)) {
                // Blame the first budget-relevant field the request changed.
                const char* field = "resolution";
                for (const char* key : {"stream_radius", "resolution", "droplets", "erosion",
                                        "erosion_steps", "streaming"}) {
                    if (f.has(key)) {
                        field = key;
                        break;
                    }
                }
                f.fail(field, over->message);
            }
            // Only a new shape regenerates (issue #38): `snowline` is
            // shading, and the runtime applies `stream_radius` (resizing the
            // window, keeping loaded tiles), `gpu` (the next tile's
            // generator) and `streaming` (a mode switch) itself.
            if (!terrain::same_shape(before, p)) t.regenerate = true;
        });
    };
    return registry.add(typed_codec<Terrain>(std::move(codec)));
}

} // namespace engine::scene::codec
