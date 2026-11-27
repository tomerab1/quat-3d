// NavMesh baking and path queries on raw triangles: a 40x40 m plate with a
// wall across z = 0 that leaves a gap on the +x side.

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/nav/navmesh.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::nav::NavMesh;

class NavMeshWall : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        std::vector<glm::vec3>     verts;
        std::vector<std::uint32_t> idx;
        const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
            const auto base = static_cast<std::uint32_t>(verts.size());
            verts.insert(verts.end(), {a, b, c, d});
            idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        };
        quad({-20.0F, 0.0F, -20.0F}, {-20.0F, 0.0F, 20.0F}, {20.0F, 0.0F, 20.0F},
             {20.0F, 0.0F, -20.0F});
        // Wall: a thin box over x in [-20, 12], 3 m tall, 1 m thick, centred on z=0.
        const float x0 = -20.0F, x1 = 12.0F, z0 = -0.5F, z1 = 0.5F, h = 3.0F;
        quad({x0, h, z0}, {x0, h, z1}, {x1, h, z1}, {x1, h, z0});       // top
        quad({x0, 0.0F, z0}, {x0, h, z0}, {x1, h, z0}, {x1, 0.0F, z0}); // -z face
        quad({x1, 0.0F, z1}, {x1, h, z1}, {x0, h, z1}, {x0, 0.0F, z1}); // +z face
        quad({x1, 0.0F, z0}, {x1, h, z0}, {x1, h, z1}, {x1, 0.0F, z1}); // +x cap

        auto built = NavMesh::build(verts, idx);
        ASSERT_OK(built);
        mesh().emplace(std::move(*built));
    }
    static void TearDownTestSuite() { mesh().reset(); }

    static std::optional<NavMesh>& mesh() {
        static std::optional<NavMesh> m;
        return m;
    }

    void SetUp() override { ASSERT_TRUE(mesh().has_value()) << "the bake failed"; }

    // A route from one side of the wall to the other.
    [[nodiscard]] static std::vector<glm::vec3> path_across() {
        auto path = mesh()->find_path({-10.0F, 0.0F, -10.0F}, {-10.0F, 0.0F, 10.0F});
        EXPECT_OK(path);
        return path ? std::move(*path) : std::vector<glm::vec3>{};
    }
};

TEST_F(NavMeshWall, BakeProducesDebugEdges) { EXPECT_FALSE(mesh()->debug_edges().empty()); }

// The route must detour through the gap on the +x side, not go straight
// through the wall.
TEST_F(NavMeshWall, PathDetoursThroughTheGap) {
    const std::vector<glm::vec3> path = path_across();
    ASSERT_GE(path.size(), 3U) << "path suspiciously direct through the wall";
    float max_x = std::numeric_limits<float>::lowest();
    for (const glm::vec3& p : path) max_x = glm::max(max_x, p.x);
    EXPECT_GE(max_x, 10.0F) << "path did not use the gap";
}

TEST_F(NavMeshWall, PathStaysOnThePlate) {
    const std::vector<glm::vec3> path = path_across();
    ASSERT_FALSE(path.empty());
    for (const glm::vec3& p : path) EXPECT_LE(p.y, 1.0F) << "path climbed the wall";
}

} // namespace
