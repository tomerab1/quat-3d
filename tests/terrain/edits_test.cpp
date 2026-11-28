// Terrain edit layers (G19.1): stamp validation and coverage, the ordered
// list, and per-tile application — order dependence, region-limited
// recompute equal to a whole-tile recompute, and seams between tiles.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/terrain/edits.hpp"
#include "engine/terrain/heightmap.hpp"

namespace {

using namespace engine::terrain;

// A bumpy tile: smooth sines so blur and flatten have something to do.
Heightmap bumpy_tile(const TileFrame& frame) {
    Heightmap map;
    map.resolution = frame.resolution;
    map.tile_size_m = frame.tile_size_m;
    map.heights.resize(static_cast<std::size_t>(frame.resolution) * frame.resolution);
    for (std::uint32_t z = 0; z < frame.resolution; ++z) {
        for (std::uint32_t x = 0; x < frame.resolution; ++x) {
            const glm::vec2 p = frame.texel_position(x, z);
            map.heights[static_cast<std::size_t>(z) * frame.resolution + x] =
                4.0F * std::sin(p.x * 0.11F) * std::cos(p.y * 0.07F) + 0.02F * p.x;
        }
    }
    map.min_height = -10.0F;
    map.max_height = 10.0F;
    return map;
}

TerrainStamp circle(glm::vec2 c, float r, float falloff, StampOp op) {
    TerrainStamp s;
    s.shape = StampShape::circle;
    s.points = {c};
    s.radius = r;
    s.falloff = falloff;
    s.op = op;
    return s;
}

TerrainStamp polygon(std::vector<glm::vec2> ring, float falloff, StampOp op) {
    TerrainStamp s;
    s.shape = StampShape::polygon;
    s.points = std::move(ring);
    s.falloff = falloff;
    s.op = op;
    return s;
}

TerrainStamp capsule(std::vector<glm::vec2> line, float r, float falloff, StampOp op) {
    TerrainStamp s;
    s.shape = StampShape::capsule;
    s.points = std::move(line);
    s.radius = r;
    s.falloff = falloff;
    s.op = op;
    return s;
}

// A mixed list touching the tile: flatten plinth, path paint + mask, raise,
// smooth over both, a tilted plane.
std::vector<TerrainStamp> mixed_list() {
    TerrainEditList list;
    TerrainStamp plinth =
        polygon({{-20, -20}, {10, -22}, {12, 8}, {-18, 10}}, 4.0F, StampOp::flatten);
    plinth.height = 1.5F;
    plinth.radius = 1.0F;
    (void)list.add(plinth);
    TerrainStamp path = capsule({{-60, 30}, {-10, 20}, {30, 35}, {60, 10}}, 2.0F, 2.0F,
                                StampOp::paint);
    path.channel = PaintChannel::dirt;
    path.value = 1.0F;
    (void)list.add(path);
    TerrainStamp mask = path;
    mask.channel = PaintChannel::grass;
    mask.value = 0.0F;
    (void)list.add(mask);
    TerrainStamp bump = circle({35, -30}, 6.0F, 8.0F, StampOp::raise);
    bump.amount = 3.0F;
    (void)list.add(bump);
    TerrainStamp blur = capsule({{-60, 30}, {-10, 20}, {30, 35}}, 3.0F, 3.0F, StampOp::smooth);
    blur.iterations = 3;
    blur.strength = 0.8F;
    (void)list.add(blur);
    TerrainStamp ramp = circle({-40, -40}, 8.0F, 5.0F, StampOp::flatten);
    ramp.normal = glm::vec3(0.2F, 1.0F, 0.1F);
    ramp.pivot = {-40, -40};
    ramp.height = -2.0F;
    (void)list.add(ramp);
    return list.stamps();
}

constexpr TileFrame k_frame{{0, 0}, 129, 160.0F};

void expect_same(const Heightmap& a, const Heightmap& b) {
    ASSERT_EQ(a.heights.size(), b.heights.size());
    for (std::size_t i = 0; i < a.heights.size(); ++i) {
        ASSERT_EQ(a.heights[i], b.heights[i]) << "texel " << i;
    }
    EXPECT_EQ(a.paint, b.paint);
    EXPECT_EQ(a.edit_cells, b.edit_cells);
    EXPECT_EQ(a.min_height, b.min_height);
    EXPECT_EQ(a.max_height, b.max_height);
}

TEST(TerrainEdits, ValidationNamesTheField) {
    TerrainStamp s = circle({0, 0}, 0.0F, 0.0F, StampOp::raise);
    auto r = validate_stamp(s);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("radius"), std::string::npos);
    s = polygon({{0, 0}, {1, 0}}, 1.0F, StampOp::flatten);
    r = validate_stamp(s);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("points"), std::string::npos);
    s = circle({0, 0}, 1.0F, 0.0F, StampOp::paint);
    s.value = 2.0F;
    EXPECT_FALSE(validate_stamp(s));
    s.value = 1.0F;
    s.normal = glm::vec3(1.0F, 0.0F, 0.0F);
    EXPECT_FALSE(validate_stamp(s));
    s.normal = glm::vec3(0.0F, 1.0F, 0.0F);
    s.points[0].x = NAN;
    EXPECT_FALSE(validate_stamp(s));
    s.points[0].x = 0.0F;
    EXPECT_TRUE(validate_stamp(s));
}

TEST(TerrainEdits, CoverageOfEachShape) {
    const TerrainStamp c = circle({0, 0}, 5.0F, 2.0F, StampOp::raise);
    EXPECT_EQ(stamp_coverage(c, {0, 0}), 1.0F);
    EXPECT_EQ(stamp_coverage(c, {5, 0}), 1.0F);
    EXPECT_NEAR(stamp_coverage(c, {6, 0}), 0.5F, 1e-5F);
    EXPECT_EQ(stamp_coverage(c, {7.5F, 0}), 0.0F);

    // A concave L polygon: its notch is outside.
    const TerrainStamp l =
        polygon({{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}}, 0.0F, StampOp::flatten);
    EXPECT_EQ(stamp_coverage(l, {2, 2}), 1.0F);
    EXPECT_EQ(stamp_coverage(l, {2, 8}), 1.0F);
    EXPECT_EQ(stamp_coverage(l, {8, 8}), 0.0F);
    EXPECT_EQ(stamp_coverage(l, {-1, 5}), 0.0F);
    // Clockwise winding works the same.
    const TerrainStamp cw = polygon({{0, 0}, {0, 10}, {10, 10}, {10, 0}}, 0.0F, StampOp::flatten);
    EXPECT_EQ(stamp_coverage(cw, {5, 5}), 1.0F);
    EXPECT_EQ(stamp_coverage(cw, {11, 5}), 0.0F);

    const TerrainStamp cap = capsule({{0, 0}, {10, 0}}, 1.0F, 1.0F, StampOp::paint);
    EXPECT_EQ(stamp_coverage(cap, {5, 0.5F}), 1.0F);
    EXPECT_EQ(stamp_coverage(cap, {-1, 0}), 1.0F); // round end cap
    EXPECT_NEAR(stamp_coverage(cap, {5, 1.5F}), 0.5F, 1e-5F);
    EXPECT_EQ(stamp_coverage(cap, {5, 3}), 0.0F);
}

TEST(TerrainEdits, ListAssignsStableIdsAndOrders) {
    TerrainEditList list;
    const std::uint64_t r0 = list.revision();
    auto a = list.add(circle({0, 0}, 1, 0, StampOp::raise));
    auto b = list.add(circle({5, 0}, 1, 0, StampOp::raise));
    ASSERT_TRUE(a && b);
    EXPECT_NE(*a, *b);
    auto c = list.add(circle({9, 0}, 1, 0, StampOp::lower), *b);
    ASSERT_TRUE(c);
    ASSERT_EQ(list.stamps().size(), 3U);
    EXPECT_EQ(list.stamps()[1].id, *c); // inserted before b
    EXPECT_GT(list.revision(), r0);
    EXPECT_FALSE(list.add(circle({0, 0}, 1, 0, StampOp::raise), 999));

    const std::uint64_t r1 = list.revision();
    ASSERT_TRUE(list.update(*a, list.stamps()[0])); // unchanged: no new revision
    EXPECT_EQ(list.revision(), r1);
    EXPECT_TRUE(list.remove(*a));
    EXPECT_FALSE(list.remove(*a));
    auto d = list.add(circle({0, 0}, 1, 0, StampOp::raise));
    ASSERT_TRUE(d);
    EXPECT_GT(*d, *c); // ids never reused

    std::vector<TerrainStamp> dup = list.stamps();
    dup[1].id = dup[0].id;
    EXPECT_FALSE(TerrainEditList{}.assign(dup));
}

TEST(TerrainEdits, ChangedRegionsCoverOnlyChangedStamps) {
    const std::vector<TerrainStamp> before = mixed_list();
    std::vector<TerrainStamp> after = before;
    after[3].amount = 1.0F; // the bump
    const std::vector<EditRect> rects = changed_regions(before, after);
    ASSERT_EQ(rects.size(), 1U);
    const EditRect b = stamp_bounds(before[3]);
    EXPECT_EQ(rects[0].min, b.min);
    EXPECT_EQ(rects[0].max, b.max);
    EXPECT_TRUE(changed_regions(before, before).empty());
    std::swap(after[0], after[1]);
    EXPECT_FALSE(changed_regions(before, after).empty());
}

TEST(TerrainEdits, FlattenRaisePaintDoWhatTheySay) {
    Heightmap map = bumpy_tile(k_frame);
    TerrainStamp level = circle({0, 0}, 10.0F, 5.0F, StampOp::flatten);
    level.height = 2.5F;
    TerrainStamp paint = circle({0, 0}, 10.0F, 0.0F, StampOp::paint);
    paint.channel = PaintChannel::grass;
    paint.value = 0.0F;
    const std::vector<TerrainStamp> stamps{level, paint};
    const TexelRect done = apply_edits(map, k_frame, stamps);
    EXPECT_FALSE(done.empty());
    ASSERT_EQ(map.base_heights.size(), map.heights.size());
    ASSERT_EQ(map.paint.size(), map.heights.size());
    // The centre texel (64, 64) sits at the origin.
    const std::size_t centre = 64U * 129U + 64U;
    EXPECT_EQ(map.heights[centre], 2.5F);
    EXPECT_EQ(paint_channel(map.paint[centre], PaintChannel::grass), 0.0F);
    EXPECT_EQ(paint_channel(map.paint[0], PaintChannel::grass), 1.0F);
    EXPECT_EQ(map.heights[0], map.base_heights[0]); // far corner untouched

    TerrainStamp up = circle({0, 0}, 10.0F, 0.0F, StampOp::raise);
    up.amount = 2.0F;
    TerrainStamp down = up;
    down.op = StampOp::lower;
    const std::vector<TerrainStamp> both{level, up, down};
    Heightmap again = bumpy_tile(k_frame);
    (void)apply_edits(again, k_frame, both);
    EXPECT_EQ(again.heights[centre], 2.5F);

    // A plane through the pivot rising along +x.
    TerrainStamp plane = circle({0, 0}, 40.0F, 0.0F, StampOp::flatten);
    plane.normal = glm::normalize(glm::vec3(-0.5F, 1.0F, 0.0F)); // slope +0.5 in x
    plane.height = 1.0F;
    Heightmap tilted = bumpy_tile(k_frame);
    (void)apply_edits(tilted, k_frame, std::vector<TerrainStamp>{plane});
    const float x10 = k_frame.texel_position(72, 64).x; // 8 texels * 1.25 m
    EXPECT_NEAR(tilted.heights[64U * 129U + 72U], 1.0F + 0.5F * x10, 1e-4F);
}

TEST(TerrainEdits, OrderMatters) {
    TerrainStamp level = circle({0, 0}, 10.0F, 0.0F, StampOp::flatten);
    level.height = 0.0F;
    TerrainStamp up = circle({0, 0}, 10.0F, 0.0F, StampOp::raise);
    up.amount = 2.0F;
    Heightmap a = bumpy_tile(k_frame);
    Heightmap b = bumpy_tile(k_frame);
    (void)apply_edits(a, k_frame, std::vector<TerrainStamp>{level, up});
    (void)apply_edits(b, k_frame, std::vector<TerrainStamp>{up, level});
    const std::size_t centre = 64U * 129U + 64U;
    EXPECT_EQ(a.heights[centre], 2.0F);
    EXPECT_EQ(b.heights[centre], 0.0F);
}

TEST(TerrainEdits, RegionRecomputeEqualsFullRecompute) {
    std::vector<TerrainStamp> before = mixed_list();
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> coord(-70.0F, 70.0F);
    for (int round = 0; round < 12; ++round) {
        Heightmap incremental = bumpy_tile(k_frame);
        (void)apply_edits(incremental, k_frame, before);

        std::vector<TerrainStamp> after = before;
        const std::size_t k = static_cast<std::size_t>(round) % after.size();
        for (glm::vec2& p : after[k].points) p += glm::vec2(coord(rng), coord(rng)) * 0.2F;
        if (round % 4 == 3) after.erase(after.begin() + static_cast<std::ptrdiff_t>(k));

        for (const EditRect& r : changed_regions(before, after)) {
            (void)apply_edits(incremental, k_frame, after, texel_rect(k_frame, r));
        }
        Heightmap full = bumpy_tile(k_frame);
        (void)apply_edits(full, k_frame, after);
        SCOPED_TRACE(round);
        expect_same(incremental, full);
        before = after;
    }
}

TEST(TerrainEdits, RemovingEveryStampRestoresTheGeneratedGround) {
    const Heightmap original = bumpy_tile(k_frame);
    Heightmap map = original;
    const std::vector<TerrainStamp> stamps = mixed_list();
    (void)apply_edits(map, k_frame, stamps);
    EXPECT_NE(map.heights, original.heights);
    for (const EditRect& r : changed_regions(stamps, {})) {
        (void)apply_edits(map, k_frame, {}, texel_rect(k_frame, r));
    }
    EXPECT_EQ(map.heights, original.heights);
    for (const std::uint32_t t : map.paint) ASSERT_EQ(t, paint_default_texel);
}

TEST(TerrainEdits, NeighbouringTilesAgreeOnTheirSharedEdge) {
    const TileFrame left{{0, 0}, 65, 80.0F};
    const TileFrame right{{1, 0}, 65, 80.0F};
    // A shared edge texel position is bit-identical in both tiles.
    EXPECT_EQ(left.texel_position(64, 10), right.texel_position(0, 10));
    Heightmap a = bumpy_tile(left);
    Heightmap b = bumpy_tile(right);
    // Every op straddling the seam at x = 40.
    TerrainStamp level = circle({40, 0}, 12.0F, 6.0F, StampOp::flatten);
    level.height = 3.0F;
    level.strength = 0.7F;
    TerrainStamp blur = circle({40, 5}, 10.0F, 4.0F, StampOp::smooth);
    blur.iterations = 4;
    TerrainStamp path = capsule({{20, -30}, {60, 30}}, 2.0F, 3.0F, StampOp::paint);
    path.channel = PaintChannel::gravel;
    const std::vector<TerrainStamp> stamps{level, blur, path};
    (void)apply_edits(a, left, stamps);
    (void)apply_edits(b, right, stamps);
    for (std::uint32_t z = 0; z < 65; ++z) {
        ASSERT_EQ(a.heights[z * 65 + 64], b.heights[z * 65]) << "row " << z;
        ASSERT_EQ(a.paint[z * 65 + 64], b.paint[z * 65]) << "row " << z;
    }
}

TEST(TerrainEdits, UntouchedTileStaysUnedited) {
    Heightmap map = bumpy_tile(k_frame);
    TerrainStamp far = circle({5000, 5000}, 10.0F, 0.0F, StampOp::raise);
    far.amount = 1.0F;
    EXPECT_TRUE(apply_edits(map, k_frame, std::vector<TerrainStamp>{far}).empty());
    EXPECT_TRUE(map.base_heights.empty());
    EXPECT_TRUE(map.paint.empty());
}

} // namespace
