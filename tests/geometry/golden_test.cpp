// Cross-platform determinism (ROADMAP R23.1): generated meshes must be
// bit-identical on every compiler and OS, so their hashes are golden values.
// They were computed on Linux (clang, libc++); CI checks Windows (MSVC) and
// macOS (Apple clang, arm64). A change to a generator that alters its output
// on purpose updates the value here in the same commit.
//
// Not covered: operations that run through Clipper2 (offsets, booleans,
// repair, flat-topped roofs). vcpkg compiles Clipper2 with each platform's
// floating-point defaults, so its intersection rounding may differ in rare
// half-quantum ties (docs/geometry.md).

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "engine/geometry/mesh_ops.hpp"
#include "engine/geometry/stone.hpp"
#include "engine/geometry/straight_skeleton.hpp"
#include "engine/geometry/sweep.hpp"
#include "geometry/det_math.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
namespace asset = engine::asset;

namespace {

std::uint64_t hash_of(const std::expected<MeshData, engine::core::Error>& mesh) {
    EXPECT_TRUE(mesh.has_value()) << (mesh ? "" : mesh.error().message);
    return mesh ? mesh_hash(*mesh) : 0;
}

std::uint64_t roof_hash(const Polygon& p, std::vector<double> weights, RoofOptions options) {
    auto sk = straight_skeleton(p, weights);
    EXPECT_TRUE(sk.has_value()) << (sk ? "" : sk.error().message);
    return sk ? hash_of(roof_mesh(*sk, options)) : 0;
}

} // namespace

TEST(GeometryGolden, Roofs) {
    RoofOptions options;
    options.base_height = 2.5;
    options.height_scale = 0.75;
    const Polygon plus{{{2, 0}, {4, 0}, {4, 2}, {6, 2}, {6, 4}, {4, 4}, {4, 6}, {2, 6}, {2, 4}, {0, 4}, {0, 2}, {2, 2}}, {}};
    EXPECT_EQ(roof_hash(plus, {}, options), 0x3852045fc39ad5fdULL);
    const Polygon l_shape{{{0, 0}, {8, 0}, {8, 4}, {4, 4}, {4, 8}, {0, 8}}, {}};
    options.gable_material = 1;
    EXPECT_EQ(roof_hash(l_shape, {1, 0, 1, 1, 0, 1}, options), 0x71b3b7a6652dfe39ULL);
    const Polygon courtyard{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{{3, 3}, {3, 7}, {7, 7}, {7, 3}}}};
    EXPECT_EQ(roof_hash(courtyard, {}, options), 0x0a4850f9e0037e05ULL);
    // A curved, irregular footprint: plenty of non-axis-aligned arithmetic.
    Polygon blob;
    for (int i = 0; i < 40; ++i) {
        const double a = det::two_pi * i / 40.0;
        const double r = 6.0 + 1.5 * det::sin(3.0 * a) + 0.5 * det::cos(7.0 * a);
        blob.outer.push_back({r * det::cos(a), r * det::sin(a)});
    }
    EXPECT_EQ(roof_hash(blob, {}, options), 0xf6603854382e5726ULL);
}

TEST(GeometryGolden, SweptWall) {
    std::vector<glm::dvec3> path;
    for (int i = 0; i <= 32; ++i) {
        const double a = i / 32.0 * 3.0;
        path.push_back({a * 4.0, 0.25 * a, det::sin(a * 2.0) * 2.0});
    }
    auto frames = path_frames(path);
    ASSERT_OK(frames);
    const std::vector<glm::dvec2> profile{{-0.35, 0}, {0.35, 0}, {0.3, 1.6}, {0.15, 1.8}, {-0.15, 1.8}, {-0.3, 1.6}};
    SweepOptions options;
    options.closed_profile = true;
    options.cap_start = options.cap_end = true;
    EXPECT_EQ(hash_of(sweep(profile, *frames, options)), 0xa452c1770b827db7ULL);
}

TEST(GeometryGolden, Stones) {
    StoneOptions options;
    options.seed = 7;
    EXPECT_EQ(hash_of(generate_stone(options)), 0x74bfcfcae60e7e22ULL);
    options.seed = 1234567;
    options.roundness = 0.2F;
    options.chips = 6;
    options.subdivisions = 4;
    EXPECT_EQ(hash_of(generate_stone(options)), 0xd6902aa948a6ae94ULL);
}

TEST(GeometryGolden, TaperedExtrusion) {
    const Polygon p{{{0, 0}, {6, 0}, {6, 3}, {3, 5}, {0, 3}}, {{{2, 1}, {2, 2}, {4, 2}, {4, 1}}}};
    ExtrudeOptions options;
    options.height = 3.0;
    options.taper = 0.2;
    EXPECT_EQ(hash_of(extrude(p, options)), 0x2d9187d2254dbdd3ULL);
}
