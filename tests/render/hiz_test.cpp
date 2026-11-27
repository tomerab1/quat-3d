// Hi-Z pyramid (R12): every level holds the min / max of its footprint in
// the level above (odd extents fold the extra row / column into the last
// texel), mip 0 equals the depth buffer, and the depth bounds cover exactly
// the geometry pixels.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
namespace occ = quat::test::occlusion;

// Odd on both axes so the 3-texel footprints are exercised (61x37 -> 30x18
// -> 15x9 -> 7x4 -> 3x2 -> 1x1).
constexpr VkExtent2D k_extent{61, 37};

// Expected texel (x, y) of the level below `src` (w x h).
glm::vec2 reduce(const std::vector<glm::vec2>& src, std::uint32_t w, std::uint32_t h,
                 std::uint32_t dw, std::uint32_t dh, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t nx = (w % 2 == 1 && x == dw - 1) ? 3 : 2;
    const std::uint32_t ny = (h % 2 == 1 && y == dh - 1) ? 3 : 2;
    glm::vec2 out(std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    for (std::uint32_t j = 0; j < ny; ++j) {
        for (std::uint32_t i = 0; i < nx; ++i) {
            const std::uint32_t sx = std::min(2 * x + i, w - 1);
            const std::uint32_t sy = std::min(2 * y + j, h - 1);
            const glm::vec2 v = src[static_cast<std::size_t>(sy) * w + sx];
            out = {std::min(out.x, v.x), std::max(out.y, v.y)};
        }
    }
    return out;
}

struct HiZResult {
    occ::PyramidReadback pyramid;
    std::uint32_t        bounds_min_bits = 0;
    std::uint32_t        bounds_max_bits = 0;
};

// Two triangles at varied depths over part of the screen (the rest stays
// background, depth 1).
std::expected<HiZResult, engine::core::Error> build(quat::test::GpuContext& g) {
    std::array<asset::Vertex, 6> verts{};
    verts[0].position = {-1.0F, -1.0F, 0.30F};
    verts[1].position = {1.0F, -1.0F, 0.85F};
    verts[2].position = {-1.0F, 0.6F, 0.55F};
    verts[3].position = {0.2F, 0.1F, 0.12F};
    verts[4].position = {0.9F, 0.3F, 0.20F};
    verts[5].position = {0.4F, 0.9F, 0.16F};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 3, 4, 5};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    auto material = occ::upload_grey(g);
    if (!material) return std::unexpected(material.error());
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto hiz = r::HiZPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!hiz) return std::unexpected(hiz.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto gbuffer = mesh_pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto targets = hiz->add_to_graph(graph, gbuffer->depth, k_extent);
    if (!targets) return std::unexpected(targets.error());
    auto pyramid = occ::read_pyramid(g, graph, targets->pyramid, gbuffer->depth, k_extent,
                                     targets->mip_count);
    if (!pyramid) return std::unexpected(pyramid.error());
    if (auto inv = hiz->bounds_buffer().invalidate_mapped(); !inv) {
        return std::unexpected(inv.error());
    }
    HiZResult out;
    out.pyramid = std::move(*pyramid);
    const auto* bits = static_cast<const std::uint32_t*>(hiz->bounds_buffer().mapped());
    out.bounds_min_bits = bits[0];
    out.bounds_max_bits = bits[1];
    return out;
}

const std::expected<HiZResult, engine::core::Error>& result(quat::test::GpuContext& g) {
    static const std::expected<HiZResult, engine::core::Error> r = build(g);
    return r;
}

} // namespace

TEST(GpuHiZ, FullMipChainDownToOneTexel) {
    QUAT_REQUIRE_GPU(g);
    const auto& res = result(*g);
    ASSERT_OK(res);
    const auto& levels = res->pyramid.extents;
    ASSERT_EQ(levels.size(), 6U);
    EXPECT_EQ(levels.back().width, 1U);
    EXPECT_EQ(levels.back().height, 1U);
}

TEST(GpuHiZ, MipZeroIsTheDepthBuffer) {
    QUAT_REQUIRE_GPU(g);
    const auto& res = result(*g);
    ASSERT_OK(res);
    const auto& mip0 = res->pyramid.levels[0];
    const auto& depth = res->pyramid.depth;
    ASSERT_EQ(mip0.size(), depth.size());
    int geometry = 0;
    for (std::size_t i = 0; i < depth.size(); ++i) {
        ASSERT_EQ(mip0[i].x, depth[i]) << "texel " << i;
        ASSERT_EQ(mip0[i].y, depth[i]) << "texel " << i;
        geometry += depth[i] < 1.0F ? 1 : 0;
    }
    // The fixture must cover part of the screen, not all or none of it.
    EXPECT_GT(geometry, 100);
    EXPECT_LT(geometry, static_cast<int>(depth.size()) - 100);
}

TEST(GpuHiZ, EachLevelIsMinMaxOfTheLevelBelow) {
    QUAT_REQUIRE_GPU(g);
    const auto& res = result(*g);
    ASSERT_OK(res);
    const auto& p = res->pyramid;
    for (std::size_t m = 1; m < p.levels.size(); ++m) {
        const VkExtent2D s = p.extents[m - 1];
        const VkExtent2D d = p.extents[m];
        for (std::uint32_t y = 0; y < d.height; ++y) {
            for (std::uint32_t x = 0; x < d.width; ++x) {
                const glm::vec2 want =
                    reduce(p.levels[m - 1], s.width, s.height, d.width, d.height, x, y);
                const glm::vec2 got = p.levels[m][static_cast<std::size_t>(y) * d.width + x];
                ASSERT_EQ(got, want) << "mip " << m << " texel " << x << "," << y;
            }
        }
    }
}

TEST(GpuHiZ, TopLevelBoundsTheWholeDepthBuffer) {
    QUAT_REQUIRE_GPU(g);
    const auto& res = result(*g);
    ASSERT_OK(res);
    const auto& depth = res->pyramid.depth;
    const auto [lo, hi] = std::minmax_element(depth.begin(), depth.end());
    const glm::vec2 top = res->pyramid.levels.back()[0];
    EXPECT_EQ(top.x, *lo);
    EXPECT_EQ(top.y, *hi);
}

TEST(GpuHiZ, DepthBoundsCoverGeometryOnly) {
    QUAT_REQUIRE_GPU(g);
    const auto& res = result(*g);
    ASSERT_OK(res);
    float lo = 1.0F;
    float hi = 0.0F;
    for (float d : res->pyramid.depth) {
        if (d >= 1.0F) continue;
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    float got_lo = 0.0F;
    float got_hi = 0.0F;
    std::memcpy(&got_lo, &res->bounds_min_bits, sizeof(float));
    std::memcpy(&got_hi, &res->bounds_max_bits, sizeof(float));
    EXPECT_EQ(got_lo, lo);
    EXPECT_EQ(got_hi, hi);
    EXPECT_LT(got_hi, 1.0F);
}
