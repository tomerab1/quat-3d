// GPU legacy self-tests wrapped as GoogleTest cases (ROADMAP S3, step 1).
// They run on the shared surface-less device (support/gpu_context.hpp) and
// skip when no Vulkan device exists. Step 2 moves the bodies here.

#include <gtest/gtest.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene_io.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace scn = engine::scene;

TEST(GpuRhi, RenderGraph) {
    QUAT_REQUIRE_GPU(g);
    engine::rhi::TransientImagePool pool(g->device, g->allocator);
    ASSERT_OK(engine::rhi::run_render_graph_self_test(g->device, g->allocator, pool));
}

TEST(GpuAssets, GltfMesh) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(scn::run_gltf_loader_self_test(g->allocator, g->transfer));
}
TEST(GpuAssets, GltfTextures) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(scn::run_texture_loader_self_test(g->allocator, g->transfer));
}
TEST(GpuAssets, GltfSceneGraph) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(scn::run_gltf_scene_self_test(g->allocator, g->transfer));
}
TEST(GpuAssets, SceneSaveLoad) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(scn::run_scene_io_self_test(g->allocator, g->transfer));
}
