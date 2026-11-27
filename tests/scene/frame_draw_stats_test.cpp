// Per-frame draw / instance / triangle counters (R4 profiling plots).

#include <gtest/gtest.h>

#include <vector>

#include "engine/asset/mesh_asset.hpp"
#include "engine/scene/instanced_mesh.hpp"

namespace {

namespace s = engine::scene;
using engine::renderer::DrawItem;
using engine::renderer::InstancedDraw;

TEST(FrameDrawStats, CountsInstancesAndTrianglesPerList) {
    engine::asset::MeshAsset cube;
    cube.index_count = 36; // 12 triangles
    engine::asset::MeshAsset quad;
    quad.index_count = 6; // 2 triangles

    s::FrameDrawLists lists;
    lists.opaque.draws = {InstancedDraw{.mesh = &cube, .instance_count = 10},
                          InstancedDraw{.mesh = &quad, .instance_count = 1}};
    lists.shadow.draws = {InstancedDraw{.mesh = &cube, .instance_count = 10}};
    const std::vector<DrawItem> transparent{DrawItem{.mesh = &quad}, DrawItem{.mesh = nullptr}};

    const s::FrameDrawStats with = s::frame_draw_stats(lists, transparent, true);
    EXPECT_EQ(with.draws, 4U); // the mesh-less item draws nothing
    EXPECT_EQ(with.instances, 22U);
    EXPECT_EQ(with.triangles, 120U + 2U + 120U + 2U);

    const s::FrameDrawStats without = s::frame_draw_stats(lists, transparent, false);
    EXPECT_EQ(without.draws, 3U);
    EXPECT_EQ(without.instances, 12U);
    EXPECT_EQ(without.triangles, 124U);
}

TEST(FrameDrawStats, EmptyFrameIsZero) {
    const s::FrameDrawStats none = s::frame_draw_stats({}, {}, true);
    EXPECT_EQ(none.draws, 0U);
    EXPECT_EQ(none.instances, 0U);
    EXPECT_EQ(none.triangles, 0U);
}

} // namespace
