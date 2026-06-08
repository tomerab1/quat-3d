#include "engine/renderer/skinning_pass.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

struct SkinningPushConstants {
    std::uint32_t vertex_count = 0;
};

// Set 0: vertices-in, skin, joints (read) and vertices-out (write), all storage
// buffers. Matches skinning.slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 4> skinning_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
    }};
}

} // namespace

std::expected<SkinningPass, core::Error>
SkinningPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    SkinningPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("skinning pass: descriptor buffer functions unavailable");
    }

    const auto bindings = skinning_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/skinning.spv");
    if (!shader) return std::unexpected(shader.error());

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(SkinningPushConstants)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = rhi::ComputePipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    return out;
}

std::expected<void, core::Error>
SkinningPass::record(VkCommandBuffer cmd, std::span<const SkinDispatch> dispatches) {
    frame_descriptors_.clear();
    frame_descriptors_.reserve(dispatches.size());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());

    for (const SkinDispatch& d : dispatches) {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
        if (!db) return std::unexpected(db.error());
        db->write_storage_buffer(0, d.vertices_in, d.vertices_in_size);
        db->write_storage_buffer(1, d.skin, d.skin_size);
        db->write_storage_buffer(2, d.joints, d.joints_size);
        db->write_storage_buffer(3, d.vertices_out, d.vertices_out_size);
        frame_descriptors_.push_back(std::move(*db));

        frame_descriptors_.back().bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        SkinningPushConstants push{d.vertex_count};
        vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (d.vertex_count + 63) / 64, 1, 1);
    }

    // Make the skinned vertices visible to the GBuffer pass's vertex fetch.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
    return {};
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

template <typename Record>
[[nodiscard]] std::expected<void, core::Error>
submit(const rhi::Device& device, Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return fail("skinning self-test: vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    std::expected<void, core::Error> rec = record(cmd);
    vkEndCommandBuffer(cmd);
    if (!rec) {
        vkDestroyCommandPool(device.handle(), pool, nullptr);
        return rec;
    }

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cmd_info;
    const VkResult r = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(device.graphics_queue());
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return r == VK_SUCCESS ? std::expected<void, core::Error>{}
                           : fail("skinning self-test: vkQueueSubmit2 failed");
}

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
storage_buffer(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer, const void* data,
               VkDeviceSize size, VkBufferUsageFlags extra = 0) {
    return rhi::upload_device_buffer(allocator, transfer, data, size,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra);
}

} // namespace

std::expected<void, core::Error>
run_skinning_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                            rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                            const std::string& cooked_shader_dir) {
    const VkDevice vk = device.handle();
    auto pass = SkinningPass::create(device, allocator, cache, cooked_shader_dir);
    if (!pass) return std::unexpected(pass.error());

    // --- Part 1: skinning correctness ---------------------------------------
    // Four vertices: v0,v1 bound 100% to joint 1 (translated +0.3 X), v2,v3 to
    // joint 0 (identity). The skinned positions move accordingly; normals stay.
    std::array<asset::Vertex, 4> verts{};
    verts[0].position = {0.0F, 0.0F, 0.0F};
    verts[1].position = {0.1F, 0.2F, 0.3F};
    verts[2].position = {-0.4F, 0.5F, 0.6F};
    verts[3].position = {1.0F, -1.0F, 0.0F};

    std::array<asset::SkinVertex, 4> skin{};
    skin[0] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    skin[1] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    skin[2] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    skin[3] = {{0, 0, 0, 0}, {1, 0, 0, 0}};

    const std::array<glm::mat4, 2> joints{
        glm::mat4(1.0F), glm::translate(glm::mat4(1.0F), glm::vec3(0.3F, 0.0F, 0.0F))};

    auto vin = storage_buffer(allocator, transfer, verts.data(), sizeof(verts));
    if (!vin) return std::unexpected(vin.error());
    auto sin = storage_buffer(allocator, transfer, skin.data(), sizeof(skin));
    if (!sin) return std::unexpected(sin.error());
    auto jin = storage_buffer(allocator, transfer, joints.data(), sizeof(joints));
    if (!jin) return std::unexpected(jin.error());

    auto vout = allocator.create_buffer(
        sizeof(verts),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!vout) return std::unexpected(vout.error());

    auto readback = allocator.create_buffer(
        sizeof(verts), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());

    SkinDispatch d{};
    d.vertices_in = rhi::buffer_device_address(vk, *vin);
    d.vertices_in_size = sizeof(verts);
    d.skin = rhi::buffer_device_address(vk, *sin);
    d.skin_size = sizeof(skin);
    d.joints = rhi::buffer_device_address(vk, *jin);
    d.joints_size = sizeof(joints);
    d.vertices_out = rhi::buffer_device_address(vk, *vout);
    d.vertices_out_size = sizeof(verts);
    d.vertex_count = 4;

    const VkBuffer vout_handle = vout->handle();
    const VkBuffer readback_handle = readback->handle();
    auto r1 = submit(device, [&](VkCommandBuffer cmd) -> std::expected<void, core::Error> {
        if (auto rr = pass->record(cmd, {&d, 1}); !rr) return rr;
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
        VkBufferCopy copy{0, 0, sizeof(verts)};
        vkCmdCopyBuffer(cmd, vout_handle, readback_handle, 1, &copy);
        return {};
    });
    if (!r1) return r1;

    std::array<asset::Vertex, 4> out{};
    std::memcpy(out.data(), readback->mapped(), sizeof(out));
    const auto near = [](const glm::vec3& a, const glm::vec3& b) {
        return glm::distance(a, b) <= 1e-4F;
    };
    if (!near(out[0].position, glm::vec3(0.3F, 0.0F, 0.0F)) ||
        !near(out[1].position, glm::vec3(0.4F, 0.2F, 0.3F))) {
        return fail("skinning self-test: joint-1 vertices not translated");
    }
    if (!near(out[2].position, verts[2].position) ||
        !near(out[3].position, verts[3].position)) {
        return fail("skinning self-test: identity-joint vertices moved");
    }
    if (!near(out[0].normal, glm::vec3(0.0F, 0.0F, 1.0F))) {
        return fail("skinning self-test: normal not preserved under translation");
    }

    // --- Part 2: the GBuffer pass reads the skinned buffer ------------------
    // A triangle skinned by an identity joint (so it stays centred), rendered
    // through the GBuffer; the centre pixel must be covered.
    constexpr VkExtent2D extent{64, 64};
    std::array<asset::Vertex, 3> tri{};
    tri[0].position = {-0.5F, -0.5F, 0.0F};
    tri[1].position = {0.5F, -0.5F, 0.0F};
    tri[2].position = {0.0F, 0.5F, 0.0F};
    const std::array<asset::SkinVertex, 3> tri_skin{
        asset::SkinVertex{{0, 0, 0, 0}, {1, 0, 0, 0}}, asset::SkinVertex{{0, 0, 0, 0}, {1, 0, 0, 0}},
        asset::SkinVertex{{0, 0, 0, 0}, {1, 0, 0, 0}}};
    const std::array<glm::mat4, 1> identity_joint{glm::mat4(1.0F)};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};

    auto tvin = storage_buffer(allocator, transfer, tri.data(), sizeof(tri));
    if (!tvin) return std::unexpected(tvin.error());
    auto tsin = storage_buffer(allocator, transfer, tri_skin.data(), sizeof(tri_skin));
    if (!tsin) return std::unexpected(tsin.error());
    auto tjin = storage_buffer(allocator, transfer, identity_joint.data(), sizeof(identity_joint));
    if (!tjin) return std::unexpected(tjin.error());
    auto ibuf = rhi::upload_device_buffer(allocator, transfer, indices.data(), sizeof(indices),
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ibuf) return std::unexpected(ibuf.error());

    // Skinned output doubles as the mesh's vertex buffer (STORAGE write + VERTEX).
    auto tvout = allocator.create_buffer(
        sizeof(tri),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!tvout) return std::unexpected(tvout.error());

    asset::MeshAsset mesh;
    mesh.index_buffer = std::move(*ibuf);
    mesh.vertex_buffer = std::move(*tvout);
    mesh.vertex_count = 3;
    mesh.index_count = 3;
    mesh.submeshes = {asset::SubMesh{0, 3, 0}};

    SkinDispatch td{};
    td.vertices_in = rhi::buffer_device_address(vk, *tvin);
    td.vertices_in_size = sizeof(tri);
    td.skin = rhi::buffer_device_address(vk, *tsin);
    td.skin_size = sizeof(tri_skin);
    td.joints = rhi::buffer_device_address(vk, *tjin);
    td.joints_size = sizeof(identity_joint);
    td.vertices_out = rhi::buffer_device_address(vk, mesh.vertex_buffer);
    td.vertices_out_size = sizeof(tri);
    td.vertex_count = 3;

    asset::PbrMaterialParams params;
    params.base_color_factor = {0.25F, 0.5F, 0.75F, 1.0F};
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = MeshPass::create(device, allocator, cache, transfer, cooked_shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());

    auto albedo_readback = allocator.create_buffer(
        static_cast<VkDeviceSize>(extent.width) * extent.height * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!albedo_readback) return std::unexpected(albedo_readback.error());
    const VkBuffer albedo_handle = albedo_readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    const DrawItem item{&mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    const VkImage albedo_image = graph.binding(targets->albedo).image;

    auto r2 = submit(device, [&](VkCommandBuffer cmd) -> std::expected<void, core::Error> {
        if (auto rr = pass->record(cmd, {&td, 1}); !rr) return rr; // skinning -> barrier
        graph.execute(cmd);                                        // GBuffer reads skinned buffer

        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = albedo_image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_src;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, albedo_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               albedo_handle, 1, &region);
        return {};
    });
    if (!r2) return r2;

    const auto* px = static_cast<const std::uint8_t*>(albedo_readback->mapped());
    const std::uint8_t* centre = px + (static_cast<std::size_t>(extent.height / 2) * extent.width +
                                       extent.width / 2) * 4;
    if (centre[0] == 0 && centre[1] == 0 && centre[2] == 0) {
        return fail("skinning self-test: GBuffer did not render the skinned triangle");
    }
    return {};
}

} // namespace engine::renderer
