// The skinning compute pass never reads past the joint-matrix buffer (S1
// audit C5): JOINTS_0 indices are clamped to the skin's joint count, and a
// dispatch without joint matrices is skipped.

#include <array>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::asset::SkinVertex;
using engine::asset::Vertex;

constexpr VkBufferUsageFlags kStorage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

TEST(GpuSkinning, OutOfRangeJointIndicesAreClamped) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::SkinningPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    const VkDevice vk = g->device.handle();

    // v0 weights joint 9 of a 2-joint skin (clamped to joint 1: +X), v1 joint
    // 1 directly; both must land on joint 1's translation.
    std::array<Vertex, 2> verts{};
    verts[0].position = {0.0F, 1.0F, 0.0F};
    verts[1].position = {0.0F, 1.0F, 0.0F};
    std::array<SkinVertex, 2> skin{};
    skin[0] = {{9, 0, 0, 0}, {1, 0, 0, 0}};
    skin[1] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    // A sentinel matrix right after the two joints, inside the same buffer
    // but outside the bound range: reading it would move the vertex by +100 Y.
    const std::array<glm::mat4, 3> joints{
        glm::mat4(1.0F), glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 0.0F, 0.0F)),
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 100.0F, 0.0F))};

    auto vin = rhi::upload_device_buffer(g->allocator, g->transfer, verts.data(), sizeof(verts),
                                         kStorage);
    ASSERT_OK(vin);
    auto sin = rhi::upload_device_buffer(g->allocator, g->transfer, skin.data(), sizeof(skin),
                                         kStorage);
    ASSERT_OK(sin);
    auto jin = rhi::upload_device_buffer(g->allocator, g->transfer, joints.data(),
                                         sizeof(joints), kStorage);
    ASSERT_OK(jin);
    auto vout = g->allocator.create_buffer(sizeof(verts),
                                           kStorage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_AUTO, 0);
    ASSERT_OK(vout);
    auto readback = g->allocator.create_buffer(
        sizeof(verts), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(readback);

    r::SkinDispatch d{};
    d.vertices_in = rhi::buffer_device_address(vk, *vin);
    d.vertices_in_size = sizeof(verts);
    d.skin = rhi::buffer_device_address(vk, *sin);
    d.skin_size = sizeof(skin);
    d.joints = rhi::buffer_device_address(vk, *jin);
    d.joints_size = 2 * sizeof(glm::mat4); // the skin has two joints
    d.vertices_out = rhi::buffer_device_address(vk, *vout);
    d.vertices_out_size = sizeof(verts);
    d.vertex_count = 2;

    // A second dispatch with no joint matrices must be skipped, leaving its
    // output untouched (here: the same output, already written by the first).
    r::SkinDispatch empty = d;
    empty.joints_size = 0;
    const std::array<r::SkinDispatch, 2> dispatches{d, empty};

    const VkBuffer vout_handle = vout->handle();
    const VkBuffer readback_handle = readback->handle();
    ASSERT_OK(quat::test::submit_and_wait(g->device, [&](VkCommandBuffer cmd) {
        ASSERT_OK(pass->record(cmd, dispatches));
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
        vkCmdCopyBuffer(cmd, vout_handle, readback_handle, 1, &copy);
        quat::test::transfer_to_host_barrier(cmd);
    }));
    ASSERT_OK(readback->invalidate_mapped());

    std::array<Vertex, 2> out{};
    std::memcpy(out.data(), readback->mapped(), sizeof(out));
    for (const Vertex& v : out) {
        EXPECT_NEAR(v.position.x, 2.0F, 1e-4F);
        EXPECT_NEAR(v.position.y, 1.0F, 1e-4F) << "read a joint matrix past the skin";
        EXPECT_NEAR(v.position.z, 0.0F, 1e-4F);
    }
}

} // namespace
