// Terrain material blend rules (R26.1): rule coverage, bottom-to-top
// compositing, painted weights, height blending and the presets.

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>

#include "engine/terrain/material.hpp"

namespace {

using engine::terrain::AltitudeRef;
using engine::terrain::LayerRule;
using engine::terrain::LayerWeights;
using engine::terrain::PaintChannel;
using engine::terrain::SurfacePoint;
using engine::terrain::TerrainMaterial;

float sum(const LayerWeights& w) { return std::accumulate(w.begin(), w.end(), 0.0F); }

SurfacePoint flat_point(float height = 0.0F) {
    SurfacePoint p;
    p.height_m = height;
    p.snowline_m = 100.0F;
    p.noise = 0.5F; // no jitter
    return p;
}

// grass (0) < rock on slopes (1) < snow above the snowline (2).
TerrainMaterial three_layers() {
    TerrainMaterial m;
    m.layers.resize(3);
    m.layers[1].rule.slope_deg = {35.0F, 90.0F};
    m.layers[1].rule.slope_softness_deg = 5.0F;
    m.layers[2].rule.altitude_m = {0.0F, 100000.0F};
    m.layers[2].rule.altitude_softness_m = 10.0F;
    m.layers[2].rule.altitude_ref = AltitudeRef::snowline;
    return m;
}

TEST(TerrainMaterial, SlopeBandSelectsLayer) {
    const TerrainMaterial m = three_layers();
    SurfacePoint p = flat_point();
    p.slope_deg = 5.0F;
    LayerWeights w = engine::terrain::layer_weights(m, p);
    EXPECT_FLOAT_EQ(w[0], 1.0F);
    EXPECT_FLOAT_EQ(w[1], 0.0F);

    p.slope_deg = 60.0F;
    w = engine::terrain::layer_weights(m, p);
    EXPECT_FLOAT_EQ(w[1], 1.0F);
    EXPECT_FLOAT_EQ(w[0], 0.0F);

    p.slope_deg = 35.0F; // edge centre: half covered
    w = engine::terrain::layer_weights(m, p);
    EXPECT_NEAR(w[1], 0.5F, 1e-5F);
    EXPECT_NEAR(sum(w), 1.0F, 1e-5F);
}

TEST(TerrainMaterial, OpenSlopeEdgeDoesNotFadeFlatGround) {
    LayerRule r; // slope 0..90: open on both sides
    SurfacePoint p = flat_point();
    p.slope_deg = 0.0F;
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(r, p), 1.0F);
    p.slope_deg = 90.0F;
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(r, p), 1.0F);
}

TEST(TerrainMaterial, AltitudeFollowsSnowline) {
    const TerrainMaterial m = three_layers();
    SurfacePoint p = flat_point(150.0F); // 50 m above the snowline at 100
    EXPECT_FLOAT_EQ(engine::terrain::layer_weights(m, p)[2], 1.0F);
    p.height_m = 50.0F;
    EXPECT_FLOAT_EQ(engine::terrain::layer_weights(m, p)[2], 0.0F);
    p.snowline_m = 20.0F; // the snowline drops below the point
    EXPECT_FLOAT_EQ(engine::terrain::layer_weights(m, p)[2], 1.0F);
}

TEST(TerrainMaterial, TopLayerCoversLowerOnes) {
    const TerrainMaterial m = three_layers();
    SurfacePoint p = flat_point(200.0F);
    p.slope_deg = 60.0F; // rock slope above the snowline: snow (top) wins
    const LayerWeights w = engine::terrain::layer_weights(m, p);
    EXPECT_FLOAT_EQ(w[2], 1.0F);
    EXPECT_FLOAT_EQ(w[1], 0.0F);
    EXPECT_FLOAT_EQ(w[0], 0.0F);
}

TEST(TerrainMaterial, NoiseJittersTheBandEdge) {
    const TerrainMaterial m = three_layers();
    SurfacePoint p = flat_point();
    p.slope_deg = 35.0F;
    p.noise = 0.0F;
    const float low = engine::terrain::layer_weights(m, p)[1];
    p.noise = 1.0F;
    const float high = engine::terrain::layer_weights(m, p)[1];
    EXPECT_LT(low, 0.5F);
    EXPECT_GT(high, 0.5F);
}

TEST(TerrainMaterial, FlowAndConcavityAffinities) {
    LayerRule r;
    r.flow = 1.0F;
    SurfacePoint p = flat_point();
    p.flow = 0.1F;
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(r, p), 0.0F);
    p.flow = 0.9F;
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(r, p), 1.0F);
    r.flow = -1.0F; // dry ground only
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(r, p), 0.0F);

    LayerRule ridge;
    ridge.concavity = -1.0F;
    p.concavity = -0.8F;
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(ridge, p), 1.0F);
    p.concavity = 0.8F;
    EXPECT_FLOAT_EQ(engine::terrain::rule_coverage(ridge, p), 0.0F);
    ridge.concavity = -0.5F; // half affinity keeps half the coverage in a gully
    EXPECT_NEAR(engine::terrain::rule_coverage(ridge, p), 0.5F, 1e-6F);
}

TEST(TerrainMaterial, PaintOverridesRules) {
    TerrainMaterial m = three_layers();
    m.layers[1].rule.paint = PaintChannel::r;
    SurfacePoint p = flat_point();
    p.paint = {1.0F, 0.0F, 0.0F, 0.0F};
    LayerWeights w = engine::terrain::layer_weights(m, p);
    EXPECT_FLOAT_EQ(w[1], 1.0F);
    EXPECT_FLOAT_EQ(w[0], 0.0F);
    p.paint.r = 0.25F;
    w = engine::terrain::layer_weights(m, p);
    EXPECT_FLOAT_EQ(w[1], 0.25F);
    EXPECT_FLOAT_EQ(w[0], 0.75F);
    EXPECT_NEAR(sum(w), 1.0F, 1e-6F);
}

TEST(TerrainMaterial, HeightBlendFavoursTallerLayer) {
    LayerWeights w{};
    w[0] = 0.5F;
    w[1] = 0.5F;
    const float heights[] = {0.2F, 0.8F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    const LayerWeights crisp = engine::terrain::height_blend(w, heights, 0.1F);
    EXPECT_FLOAT_EQ(crisp[1], 1.0F); // rock 0.6 higher, depth 0.1: grass gone
    EXPECT_FLOAT_EQ(crisp[0], 0.0F);
    for (std::size_t i = 2; i < crisp.size(); ++i) EXPECT_EQ(crisp[i], 0.0F); // zero weight stays 0

    const LayerWeights soft = engine::terrain::height_blend(w, heights, 1.0F);
    EXPECT_GT(soft[0], 0.0F);
    EXPECT_GT(soft[1], soft[0]);
    EXPECT_NEAR(sum(soft), 1.0F, 1e-6F);

    const float equal[] = {0.5F, 0.5F, 0, 0, 0, 0, 0, 0};
    const LayerWeights even = engine::terrain::height_blend(w, equal, 0.2F);
    EXPECT_NEAR(even[0], 0.5F, 1e-6F);
}

TEST(TerrainMaterial, PresetsAreValid) {
    for (const std::string_view name : engine::terrain::material_preset_names) {
        const auto m = engine::terrain::material_preset(name);
        ASSERT_TRUE(m.has_value()) << name;
        EXPECT_EQ(m->preset, name);
        EXPECT_GE(m->layers.size(), 3U) << name;
        EXPECT_LE(m->layers.size(), engine::terrain::max_material_layers) << name;
        for (const auto& l : m->layers) {
            EXPECT_TRUE(engine::terrain::is_texture_set(l.texture_set)) << name << " " << l.name;
        }
        // Flat, dry, low ground is mostly the base layer.
        const LayerWeights w = engine::terrain::layer_weights(*m, flat_point());
        EXPECT_EQ(std::max_element(w.begin(), w.end()), w.begin()) << name;
        EXPECT_NEAR(sum(w), 1.0F, 1e-5F) << name;
    }
    EXPECT_TRUE(engine::terrain::material_preset("Temperate Meadow").has_value());
    EXPECT_FALSE(engine::terrain::material_preset("lunar").has_value());
    EXPECT_EQ(engine::terrain::default_terrain_material().preset, "temperate_meadow");
}

TEST(TerrainMaterial, MeadowPutsRockOnCliffsAndSnowUpHigh) {
    const TerrainMaterial m = engine::terrain::default_terrain_material();
    SurfacePoint p = flat_point();
    p.slope_deg = 55.0F;
    const LayerWeights cliff = engine::terrain::layer_weights(m, p);
    EXPECT_GT(cliff[2], 0.99F); // rock
    p.slope_deg = 10.0F;
    p.height_m = 300.0F;        // well above the snowline
    EXPECT_GT(engine::terrain::layer_weights(m, p)[3], 0.99F);
    p.height_m = 0.0F;
    p.flow = 0.95F;             // a wet gully
    p.concavity = 0.6F;
    EXPECT_GT(engine::terrain::layer_weights(m, p)[1], 0.8F);
}

} // namespace
