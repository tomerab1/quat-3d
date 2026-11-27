// The GPU skinning compute pass: vertices follow the joint they are bound to
// (positions translated, normals preserved), and the skinned buffer feeds the
// GBuffer pass as its vertex buffer within the same command buffer.

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/glm_near.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;

// A device-local storage buffer (addressable from the skinning shader).
std::expected<rhi::GpuBuffer, Error> storage_buffer(quat::test::GpuContext& g, const void* data,
                                                    VkDeviceSize size) {
    return rhi::upload_device_buffer(g.allocator, g.transfer, data, size,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
}

// Bind pose: v0, v1 bound 100% to joint 1 (translated +0.3 X); v2, v3 to
// joint 0 (identity).
const std::array<asset::Vertex, 4>& bind_pose() {
    static const std::array<asset::Vertex, 4> verts = [] {
        std::array<asset::Vertex, 4> v{};
        v[0].position = {0.0F, 0.0F, 0.0F};
        v[1].position = {0.1F, 0.2F, 0.3F};
        v[2].position = {-0.4F, 0.5F, 0.6F};
        v[3].position = {1.0F, -1.0F, 0.0F};
        return v;
    }();
    return verts;
}

// Skins bind_pose() on the GPU and reads the output vertex buffer back; run
// once for the suite.
std::expected<std::array<asset::Vertex, 4>, Error> skin_bind_pose(quat::test::GpuContext& g) {
    const VkDevice vk = g.device.handle();
    auto pass = r::SkinningPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());

    const auto& verts = bind_pose();
    std::array<asset::SkinVertex, 4> skin{};
    skin[0] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    skin[1] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    skin[2] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    skin[3] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    const std::array<glm::mat4, 2> joints{
        glm::mat4(1.0F), glm::translate(glm::mat4(1.0F), glm::vec3(0.3F, 0.0F, 0.0F))};

    auto vin = storage_buffer(g, verts.data(), sizeof(verts));
    if (!vin) return std::unexpected(vin.error());
    auto sin = storage_buffer(g, skin.data(), sizeof(skin));
    if (!sin) return std::unexpected(sin.error());
    auto jin = storage_buffer(g, joints.data(), sizeof(joints));
    if (!jin) return std::unexpected(jin.error());
    auto vout = g.allocator.create_buffer(
        sizeof(verts),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!vout) return std::unexpected(vout.error());
    auto readback = quat::test::host_readback_buffer(g.allocator, sizeof(verts));
    if (!readback) return std::unexpected(readback.error());

    r::SkinDispatch d{};
    d.vertices_in = rhi::buffer_device_address(vk, *vin);
    d.vertices_in_size = sizeof(verts);
    d.skin = rhi::buffer_device_address(vk, *sin);
    d.skin_size = sizeof(skin);
    d.joints = rhi::buffer_device_address(vk, *jin);
    d.joints_size = sizeof(joints);
    d.vertices_out = rhi::buffer_device_address(vk, *vout);
    d.vertices_out_size = sizeof(verts);
    d.vertex_count = 4;

    std::expected<void, Error> recorded;
    const VkBuffer src = vout->handle();
    const VkBuffer dst = readback->handle();
    auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        recorded = pass->record(cmd, {&d, 1});
        // Compute storage-write -> transfer-read for the copy.
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkBufferCopy copy{0, 0, sizeof(verts)};
        vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
        quat::test::transfer_to_host_barrier(cmd);
    });
    if (!recorded) return std::unexpected(recorded.error());
    if (!done) return std::unexpected(done.error());
    if (auto inv = readback->invalidate_mapped(); !inv) return std::unexpected(inv.error());
    std::array<asset::Vertex, 4> out{};
    std::memcpy(out.data(), readback->mapped(), sizeof(out));
    return out;
}

const std::expected<std::array<asset::Vertex, 4>, Error>& skinned(quat::test::GpuContext& g) {
    static const std::expected<std::array<asset::Vertex, 4>, Error> out = skin_bind_pose(g);
    return out;
}

} // namespace

TEST(GpuSkinning, VerticesFollowTheirTranslatedJoint) {
    QUAT_REQUIRE_GPU(g);
    const auto& out = skinned(*g);
    ASSERT_OK(out);
    EXPECT_VEC_NEAR((*out)[0].position, glm::vec3(0.3F, 0.0F, 0.0F), 1e-4F);
    EXPECT_VEC_NEAR((*out)[1].position, glm::vec3(0.4F, 0.2F, 0.3F), 1e-4F);
}

TEST(GpuSkinning, IdentityJointVerticesStayPut) {
    QUAT_REQUIRE_GPU(g);
    const auto& out = skinned(*g);
    ASSERT_OK(out);
    EXPECT_VEC_NEAR((*out)[2].position, bind_pose()[2].position, 1e-4F);
    EXPECT_VEC_NEAR((*out)[3].position, bind_pose()[3].position, 1e-4F);
}

TEST(GpuSkinning, TranslationPreservesTheNormal) {
    QUAT_REQUIRE_GPU(g);
    const auto& out = skinned(*g);
    ASSERT_OK(out);
    EXPECT_VEC_NEAR((*out)[0].normal, glm::vec3(0.0F, 0.0F, 1.0F), 1e-4F);
}

// A triangle skinned by an identity joint (so it stays centred) is rendered
// through the GBuffer from the skinned output buffer, recorded right after the
// skinning dispatch (whose trailing barrier guards the vertex fetch): the
// centre pixel is covered.
TEST(GpuSkinning, GBufferDrawsTheSkinnedVertexBuffer) {
    QUAT_REQUIRE_GPU(g);
    const VkDevice vk = g->device.handle();
    auto pass = r::SkinningPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);

    std::array<asset::Vertex, 3> tri{};
    tri[0].position = {-0.5F, -0.5F, 0.0F};
    tri[1].position = {0.5F, -0.5F, 0.0F};
    tri[2].position = {0.0F, 0.5F, 0.0F};
    const asset::SkinVertex identity{{0, 0, 0, 0}, {1, 0, 0, 0}};
    const std::array<asset::SkinVertex, 3> tri_skin{identity, identity, identity};
    const std::array<glm::mat4, 1> identity_joint{glm::mat4(1.0F)};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};

    auto vin = storage_buffer(*g, tri.data(), sizeof(tri));
    ASSERT_OK(vin);
    auto sin = storage_buffer(*g, tri_skin.data(), sizeof(tri_skin));
    ASSERT_OK(sin);
    auto jin = storage_buffer(*g, identity_joint.data(), sizeof(identity_joint));
    ASSERT_OK(jin);
    auto ibuf = rhi::upload_device_buffer(g->allocator, g->transfer, indices.data(),
                                          sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    ASSERT_OK(ibuf);
    // The skinned output doubles as the mesh's vertex buffer (STORAGE write + VERTEX).
    auto vout = g->allocator.create_buffer(
        sizeof(tri),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    ASSERT_OK(vout);

    asset::MeshAsset mesh;
    mesh.index_buffer = std::move(*ibuf);
    mesh.vertex_buffer = std::move(*vout);
    mesh.vertex_count = 3;
    mesh.index_count = 3;
    mesh.submeshes = {asset::SubMesh{0, 3, 0}};

    r::SkinDispatch d{};
    d.vertices_in = rhi::buffer_device_address(vk, *vin);
    d.vertices_in_size = sizeof(tri);
    d.skin = rhi::buffer_device_address(vk, *sin);
    d.skin_size = sizeof(tri_skin);
    d.joints = rhi::buffer_device_address(vk, *jin);
    d.joints_size = sizeof(identity_joint);
    d.vertices_out = rhi::buffer_device_address(vk, mesh.vertex_buffer);
    d.vertices_out_size = sizeof(tri);
    d.vertex_count = 3;

    asset::PbrMaterialParams params;
    params.base_color_factor = {0.25F, 0.5F, 0.75F, 1.0F};
    auto material =
        asset::upload_material(params, asset::MaterialTextures{}, g->allocator, g->transfer);
    ASSERT_OK(material);
    auto mesh_pass =
        r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh_pass);

    constexpr VkExtent2D extent{64, 64};
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, extent, glm::mat4(1.0F), {&item, 1});
    ASSERT_OK(targets);
    std::expected<void, Error> recorded;
    auto albedo = quat::test::render_to_host(
        *g, graph, targets->albedo, extent, 4,
        [&](VkCommandBuffer cmd) { recorded = pass->record(cmd, {&d, 1}); });
    ASSERT_OK(recorded);
    ASSERT_OK(albedo);
    const glm::uvec2 c = albedo->centre();
    EXPECT_NE(glm::ivec3(albedo->rgba8(c.x, c.y)), glm::ivec3(0))
        << "GBuffer did not render the skinned triangle";
}
