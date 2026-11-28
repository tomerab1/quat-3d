// The mountain-forest showcase (assets/scenes/mountain_forest.json, QUAT_DEMO=
// mountain): the file decodes, its terrain generates, every scatter layer
// places a plausible number of instances, and every placement honours its
// layer's slope / height limits on the real heightmap (trees stay below the
// treeline, off cliffs, seated on the ground).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/render_settings_json.hpp"
#include "engine/scene/scatter.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/terrain/generator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;

const std::filesystem::path k_scene_file =
    std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/scenes/mountain_forest.json";

// The tree line the forest layers respect (metres, world Y) and the steepest
// ground a tree may stand on (degrees; cliffs are steeper).
constexpr float k_treeline = 500.0F;
constexpr float k_tree_max_slope = 40.0F;

// Instance counts each layer is expected to produce on the scene's terrain.
// Generation is deterministic; the ranges leave room for last-bit terrain
// differences between compilers and for small art tweaks. Total ~10k.
struct Expected {
    std::size_t min;
    std::size_t max;
};
const std::map<std::string, Expected> k_expected = {
    {"forest_spruce", {100, 400}},         {"forest_spruce_far_e", {2500, 6000}},
    {"forest_spruce_far_n", {600, 1500}},  {"forest_fir", {70, 300}},
    {"forest_fir_far_e", {1500, 3600}},    {"forest_fir_far_n", {300, 900}},
    {"forest_pine_far_e", {600, 1600}},    {"forest_pine_far_n", {250, 700}},
    {"valley_oak", {120, 350}},            {"valley_birch", {60, 200}},
    {"rocks_boulders", {350, 950}},        {"rocks_scree", {50, 250}},
    {"rocks_spires", {180, 500}},          {"forest_edge_bushes", {100, 300}},
    {"meadow_copse", {10, 30}},            {"meadow_rocks", {6, 25}},
};

struct Layer {
    std::string name;
    s::Scatter  scatter;
    glm::mat4   world{1.0F};
};

// The scene file decoded once for the suite (terrain generation takes a few
// seconds).
struct SceneData {
    bool                      parsed = false;
    std::vector<std::string>  errors;
    std::vector<Layer>        layers;
    s::Terrain                terrain;
    glm::vec3                 terrain_origin{0.0F};
    glm::vec3                 camera{0.0F};
    int                       suns = 0;
    float                     grass_max_slope_deg = 90.0F; // the meadow grass layer's limit
    bool                      has_render_settings = false;
    engine::renderer::RenderSettings render_settings;
    engine::terrain::Heightmap map;

    // World-space ground height under (x, z).
    [[nodiscard]] float ground_at(float x, float z) const {
        const float half = map.tile_size_m * 0.5F;
        return terrain_origin.y +
               map.sample(x - (terrain_origin.x - half), z - (terrain_origin.z - half));
    }
    [[nodiscard]] s::ScatterGround ground() const {
        const float half = map.tile_size_m * 0.5F;
        return s::make_tile_ground({{&map, terrain_origin - glm::vec3(half, 0.0F, half)}});
    }
};

const SceneData& scene_data() {
    static const SceneData data = [] {
        SceneData d;
        std::ifstream in(k_scene_file);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object() || !doc.contains("entities") ||
            !doc["entities"].is_array()) {
            return d;
        }
        d.parsed = true;
        if (const auto it = doc.find("render_settings"); it != doc.end() && it->is_object()) {
            nlohmann::json block = *it;
            block.erase("version");
            if (auto ok = s::apply_render_settings_json(d.render_settings, block, true); !ok) {
                d.errors.push_back("render_settings: " + ok.error().message);
            } else {
                d.has_render_settings = true;
            }
        }
        entt::registry r;
        for (const nlohmann::json& je : doc["entities"]) {
            const entt::entity e = r.create();
            r.emplace<s::Transform>(e);
            std::string name;
            for (const auto& [key, value] : je.items()) {
                if (key == "name") {
                    if (value.is_string()) name = value.get<std::string>();
                    continue;
                }
                if (key == "parent") continue;
                if (auto ok = s::apply_component(r, e, key, value, true); !ok) {
                    d.errors.push_back(key + ": " + ok.error().message);
                }
            }
            const glm::mat4& local = r.get<s::Transform>(e).local;
            if (const auto* t = r.try_get<s::Terrain>(e)) {
                d.terrain = *t;
                d.terrain_origin = glm::vec3(local[3]);
            }
            if (const auto* sc = r.try_get<s::Scatter>(e)) d.layers.push_back({name, *sc, local});
            if (r.all_of<s::DirectionalLight>(e)) ++d.suns;
            if (const auto* gr = r.try_get<s::Grass>(e)) {
                d.grass_max_slope_deg = gr->settings.slope_deg.y;
            }
            if (const auto* c = r.try_get<s::Camera>(e); c != nullptr && c->is_active) {
                d.camera = glm::vec3(local[3]);
            }
        }
        d.map = engine::terrain::generate(d.terrain.params);
        return d;
    }();
    return data;
}

TEST(MountainScene, FileDecodesStrictly) {
    const SceneData& d = scene_data();
    ASSERT_TRUE(d.parsed) << k_scene_file << " is missing or not a scene file";
    EXPECT_TRUE(d.errors.empty()) << (d.errors.empty() ? "" : d.errors.front());
    EXPECT_EQ(d.layers.size(), k_expected.size());
    // Exactly one sun: the app adds its default sun only to scenes without
    // one, and the renderer lights with a single directional light.
    EXPECT_EQ(d.suns, 1);
    // Its render_settings block: the cascaded shadows reach past the copse
    // and the forest edge (the default 150 m ends before them).
    ASSERT_TRUE(d.has_render_settings);
    EXPECT_GE(d.render_settings.shadows.max_distance, 400.0F);
    ASSERT_TRUE(d.map.valid());
    EXPECT_GE(d.terrain.snowline_m, k_treeline) << "snow starts above the forest";
    EXPECT_LT(d.terrain.snowline_m, d.map.max_height) << "the peaks should carry snow";
    // The camera stands just above the meadow.
    const float eye = d.camera.y - d.ground_at(d.camera.x, d.camera.z);
    EXPECT_GT(eye, 0.5F);
    EXPECT_LT(eye, 8.0F);
}

// The terrain runs the multigrid talus relaxation (#69) on a 2^n + 1 tile:
// other sizes stop coarsening early and barely relax (issue #95). Relaxed,
// almost no neighbour pair stays steeper than the talus (~59 degrees here,
// 6.7 % of pairs without relaxation), yet the peaks keep their cliffs: the
// talus is set so most ground above the snowline stays past 50 degrees.
TEST(MountainScene, TerrainRelaxesOnAFullPyramidAndKeepsSteepPeaks) {
    const SceneData& d = scene_data();
    ASSERT_TRUE(d.map.valid());
    const engine::terrain::TerrainParams& p = d.terrain.params;
    const std::uint32_t cells = p.resolution - 1U;
    EXPECT_TRUE(cells != 0U && (cells & (cells - 1U)) == 0U)
        << "resolution " << p.resolution << " is not 2^n + 1";
    EXPECT_GT(p.grid.relax_cycles, 0U);

    const engine::terrain::Heightmap& m = d.map;
    const auto n = static_cast<std::int32_t>(m.resolution);
    const float dx = m.tile_size_m / static_cast<float>(m.resolution - 1U);
    const float limit = 1.1F * p.grid.talus * dx;
    std::size_t pairs = 0;
    std::size_t steep = 0;
    std::size_t high = 0;
    std::size_t high_cliff = 0;
    for (std::int32_t z = 0; z < n; ++z) {
        for (std::int32_t x = 0; x < n; ++x) {
            const float h = m.at(x, z);
            if (x + 1 < n) {
                ++pairs;
                if (std::abs(m.at(x + 1, z) - h) > limit) ++steep;
            }
            if (z + 1 < n) {
                ++pairs;
                if (std::abs(m.at(x, z + 1) - h) > limit) ++steep;
            }
            if (x == 0 || z == 0 || x + 1 == n || z + 1 == n || h < d.terrain.snowline_m) {
                continue;
            }
            const float gx = (m.at(x + 1, z) - m.at(x - 1, z)) / (2.0F * dx);
            const float gz = (m.at(x, z + 1) - m.at(x, z - 1)) / (2.0F * dx);
            ++high;
            if (std::atan(std::sqrt(gx * gx + gz * gz)) > glm::radians(50.0F)) ++high_cliff;
        }
    }
    ASSERT_GT(pairs, 0U);
    ASSERT_GT(high, 0U);
    EXPECT_LT(static_cast<double>(steep) / static_cast<double>(pairs), 0.03);
    EXPECT_GT(static_cast<double>(high_cliff) / static_cast<double>(high), 0.6);
}

TEST(MountainScene, LayersPlaceTheExpectedInstanceCounts) {
    const SceneData& d = scene_data();
    ASSERT_TRUE(d.map.valid());
    const s::ScatterGround g = d.ground();
    std::size_t total = 0;
    for (const Layer& layer : d.layers) {
        const auto placements = s::generate_scatter(layer.scatter, layer.world, g);
        total += placements.size();
        std::printf("[ mountain ] %-24s %6zu instances\n", layer.name.c_str(), placements.size());
        const auto it = k_expected.find(layer.name);
        if (it == k_expected.end()) {
            ADD_FAILURE() << "unexpected layer " << layer.name;
            continue;
        }
        EXPECT_GE(placements.size(), it->second.min) << layer.name;
        EXPECT_LE(placements.size(), it->second.max) << layer.name;
    }
    std::printf("[ mountain ] total %zu instances\n", total);
}

// The meadow in front of the camera stays under the grass layer's slope
// limit (issue #116): stronger hydraulic erosion plus the ~59-degree talus
// had carved 40-55 degree micro-relief into it, which the grass skipped and
// the terrain shaded as rock, leaving bare faceted patches. Measured on the
// heightmap with central differences, within 80 m of the camera in XZ.
TEST(MountainScene, MeadowAroundTheCameraStaysUnderTheGrassSlopeLimit) {
    const SceneData& d = scene_data();
    ASSERT_TRUE(d.map.valid());
    ASSERT_LT(d.grass_max_slope_deg, 90.0F) << "the scene's grass layer has no slope limit";
    const engine::terrain::Heightmap& m = d.map;
    const auto n = static_cast<std::int32_t>(m.resolution);
    const float dx = m.tile_size_m / static_cast<float>(m.resolution - 1U);
    const float half = m.tile_size_m * 0.5F;
    const float limit = glm::radians(d.grass_max_slope_deg);
    std::size_t texels = 0;
    std::size_t steep = 0;
    for (std::int32_t z = 1; z + 1 < n; ++z) {
        for (std::int32_t x = 1; x + 1 < n; ++x) {
            const float wx = d.terrain_origin.x - half + static_cast<float>(x) * dx;
            const float wz = d.terrain_origin.z - half + static_cast<float>(z) * dx;
            if (glm::length(glm::vec2(wx - d.camera.x, wz - d.camera.z)) > 80.0F) continue;
            const float gx = (m.at(x + 1, z) - m.at(x - 1, z)) / (2.0F * dx);
            const float gz = (m.at(x, z + 1) - m.at(x, z - 1)) / (2.0F * dx);
            ++texels;
            if (std::atan(std::sqrt(gx * gx + gz * gz)) > limit) ++steep;
        }
    }
    ASSERT_GT(texels, 500U);
    const double share = static_cast<double>(steep) / static_cast<double>(texels);
    std::printf("[ mountain ] meadow texels steeper than %.0f deg: %.2f %%\n",
                static_cast<double>(d.grass_max_slope_deg), 100.0 * share);
    EXPECT_LT(share, 0.05);
}

// The cliff-rock layers (boulders, spires) stay out of the meadow in front
// of the camera: steep micro-relief there would otherwise put a rock against
// the lens. Only meadow_rocks is placed in the foreground on purpose.
TEST(MountainScene, CliffRocksStayOutOfTheForeground) {
    const SceneData& d = scene_data();
    ASSERT_TRUE(d.map.valid());
    constexpr float k_clear_radius = 120.0F; // metres around the camera, in XZ
    const s::ScatterGround g = d.ground();
    std::size_t checked = 0;
    for (const Layer& layer : d.layers) {
        if (layer.name != "rocks_boulders" && layer.name != "rocks_spires") continue;
        SCOPED_TRACE(layer.name);
        for (const s::InstanceTransform& t : s::generate_scatter(layer.scatter, layer.world, g)) {
            const glm::vec3 p(layer.world * glm::vec4(t.position, 1.0F));
            ++checked;
            ASSERT_GT(glm::length(glm::vec2(p.x - d.camera.x, p.z - d.camera.z)), k_clear_radius)
                << "rock at " << p.x << ", " << p.z;
        }
    }
    EXPECT_GT(checked, 0U);
}

TEST(MountainScene, PlacementsHonourTheirSlopeAndHeightLimits) {
    const SceneData& d = scene_data();
    ASSERT_TRUE(d.map.valid());
    const s::ScatterGround g = d.ground();
    for (const Layer& layer : d.layers) {
        SCOPED_TRACE(layer.name);
        const bool tree = layer.name.starts_with("forest_") || layer.name.starts_with("valley_") ||
                          layer.name == "meadow_copse";
        for (const s::InstanceTransform& t : s::generate_scatter(layer.scatter, layer.world, g)) {
            const glm::vec3 p(layer.world * glm::vec4(t.position, 1.0F));
            const float h = d.ground_at(p.x, p.z);
            // Seated on the ground (base_offset 0), within the height band.
            ASSERT_NEAR(p.y, h, 0.05F) << "floating or buried at " << p.x << ", " << p.z;
            ASSERT_GE(h, layer.scatter.height_m.x - 0.01F);
            ASSERT_LE(h, layer.scatter.height_m.y + 0.01F);
            const auto sample = g.sample(p.x, p.z);
            ASSERT_TRUE(sample.has_value());
            const float slope = glm::degrees(std::acos(glm::clamp(sample->normal.y, -1.0F, 1.0F)));
            ASSERT_GE(slope, layer.scatter.slope_deg.x - 0.01F);
            ASSERT_LE(slope, layer.scatter.slope_deg.y + 0.01F);
            if (tree) {
                ASSERT_LT(h, k_treeline) << "tree above the treeline";
                ASSERT_LE(slope, k_tree_max_slope + 0.01F) << "tree on a cliff";
            }
        }
    }
}

// The file loads through the real scene loader, and every instanced model
// resolves (the glTF files exist and pass the loader).
TEST(GpuMountainScene, LoadsAndResolvesItsModels) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    engine::renderer::RenderSettings settings;
    ASSERT_OK(s::load_scene(scene, k_scene_file, g->allocator, g->transfer, assets, &settings));
    entt::registry& r = scene.registry();
    EXPECT_EQ(r.view<s::Terrain>().size(), 1U);
    EXPECT_EQ(r.view<s::Grass>().size(), 1U);
    EXPECT_EQ(r.view<s::DirectionalLight>().size(), 1U);
    // The file's render_settings block reaches the renderer settings.
    EXPECT_GE(settings.shadows.max_distance, 400.0F);
    EXPECT_EQ(r.view<s::Scatter>().size(), k_expected.size());
    const auto resolved = s::resolve_instanced_meshes(r, g->allocator, g->transfer, assets,
                                                      QUAT_PROJECT_ROOT);
    ASSERT_OK(resolved);
    EXPECT_EQ(*resolved, k_expected.size());
    for (const auto [e, parts] : r.view<s::InstancedMeshParts>().each()) {
        EXPECT_FALSE(parts.parts.empty());
        EXPECT_GT(parts.bounds.w, 0.3F);
    }
}

} // namespace
