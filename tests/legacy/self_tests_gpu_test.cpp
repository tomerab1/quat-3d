// GPU legacy self-tests wrapped as GoogleTest cases (ROADMAP S3, step 1).
// They run on the shared surface-less device (support/gpu_context.hpp) and
// skip when no Vulkan device exists. Step 2 moves the bodies here.

#include <gtest/gtest.h>

#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/dynamic_ibl.hpp"
#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene_io.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

#ifdef ENGINE_EDITOR
#include "engine/renderer/imgui_pass.hpp"
#endif

namespace r = engine::renderer;
namespace scn = engine::scene;

TEST(GpuRhi, BufferRoundTrip) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(engine::rhi::run_gpu_round_trip_self_test(g->device, g->allocator));
}
TEST(GpuRhi, DescriptorBuffer) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(engine::rhi::run_descriptor_buffer_self_test(g->device, g->allocator));
}
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

// Passes taking (device, allocator, cache, transfer, shader_dir).
#define QUAT_PASS_TEST(suite, name, fn)                                                    \
    TEST(suite, name) {                                                                    \
        QUAT_REQUIRE_GPU(g);                                                               \
        ASSERT_OK(fn(g->device, g->allocator, g->cache, g->transfer, g->shader_dir));      \
    }
// Passes taking (device, allocator, cache, shader_dir).
#define QUAT_SHADER_TEST(suite, name, fn)                                                  \
    TEST(suite, name) {                                                                    \
        QUAT_REQUIRE_GPU(g);                                                               \
        ASSERT_OK(fn(g->device, g->allocator, g->cache, g->shader_dir));                   \
    }

QUAT_PASS_TEST(GpuRender, GBuffer, r::run_mesh_pass_self_test)
QUAT_PASS_TEST(GpuRender, Lighting, r::run_lighting_pass_self_test)
QUAT_PASS_TEST(GpuRender, PointLights, r::run_point_light_self_test)
QUAT_PASS_TEST(GpuRender, Clearcoat, r::run_clearcoat_self_test)
QUAT_PASS_TEST(GpuRender, SpecularAntiAliasing, r::run_specular_aa_self_test)
QUAT_PASS_TEST(GpuRender, Terrain, r::run_terrain_pass_self_test)
QUAT_PASS_TEST(GpuRender, IblLighting, r::run_ibl_lighting_self_test)
QUAT_PASS_TEST(GpuRender, Tonemap, r::run_tonemap_pass_self_test)
QUAT_PASS_TEST(GpuRender, Skinning, r::run_skinning_pass_self_test)
#ifdef ENGINE_EDITOR
QUAT_PASS_TEST(GpuRender, ImGui, r::run_imgui_pass_self_test)
#endif

QUAT_SHADER_TEST(GpuRender, Atmosphere, r::run_atmosphere_self_test)
QUAT_SHADER_TEST(GpuRender, Ibl, r::run_ibl_self_test)
QUAT_SHADER_TEST(GpuRender, DynamicIbl, r::run_dynamic_ibl_self_test)
QUAT_SHADER_TEST(GpuRender, Bloom, r::run_bloom_self_test)
QUAT_SHADER_TEST(GpuRender, Exposure, r::run_exposure_self_test)
QUAT_SHADER_TEST(GpuRender, Taa, r::run_taa_self_test)
