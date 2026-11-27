// MeshPass self-tests (legacy run_*_self_test functions, wrapped by
// tests/legacy/self_tests_gpu_test.cpp until they migrate into GoogleTest).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

// Records `record` into a one-time primary command buffer on the graphics queue
// and blocks until it completes. The GBuffer pass issues graphics commands, so
// (unlike the transfer-only upload path) it must run on a graphics queue.
template <typename Record>
[[nodiscard]] std::expected<void, core::Error>
run_graphics_commands(const rhi::Device& device, Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return fail("mesh pass self-test: vkCreateCommandPool failed");
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
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    const VkResult r = vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(device.graphics_queue());
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return r == VK_SUCCESS ? std::expected<void, core::Error>{}
                           : fail("mesh pass self-test: vkQueueSubmit2 failed");
}

} // namespace

std::expected<void, core::Error>
run_mesh_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                        const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{64, 64};
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    // A camera-facing triangle in clip space (identity view_proj + model). The
    // image centre lands inside it; corners stay at the clear colour.
    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-0.5F, -0.5F, 0.0F};
    verts[1].position = {0.5F, -0.5F, 0.0F};
    verts[2].position = {0.0F, 0.5F, 0.0F};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};

    auto vbuf = rhi::upload_device_buffer(allocator, transfer, verts.data(),
                                          verts.size() * sizeof(asset::Vertex),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vbuf) return std::unexpected(vbuf.error());
    auto ibuf = rhi::upload_device_buffer(allocator, transfer, indices.data(),
                                          indices.size() * sizeof(std::uint32_t),
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ibuf) return std::unexpected(ibuf.error());

    asset::MeshAsset mesh;
    mesh.vertex_buffer = std::move(*vbuf);
    mesh.index_buffer = std::move(*ibuf);
    mesh.vertex_count = 3;
    mesh.index_count = 3;
    mesh.submeshes = {asset::SubMesh{0, 3, 0}};

    // Material: a known linear base colour, no textures (flags = 0).
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.25F, 0.5F, 0.75F, 1.0F};
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto pass = MeshPass::create(device, allocator, cache, transfer, cooked_shader_dir);
    if (!pass) return std::unexpected(pass.error());

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    const DrawItem item{&mesh, &*material, glm::mat4(1.0F)};
    auto targets = pass->add_to_graph(graph, extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    if (auto compiled = graph.compile(); !compiled) return std::unexpected(compiled.error());

    const VkImage albedo_image = graph.binding(targets->albedo).image;
    auto submitted = run_graphics_commands(device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);

        // The GBuffer pass left albedo in COLOR_ATTACHMENT_OPTIMAL; move it to
        // TRANSFER_SRC for the read-back copy.
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
                               readback_buffer, 1, &region);
    });
    if (!submitted) return std::unexpected(submitted.error());

    const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return px + (static_cast<std::size_t>(y) * extent.width + x) * 4;
    };
    const auto near = [](std::uint8_t v, int target) { return std::abs(int(v) - target) <= 2; };

    // Centre is covered by the triangle: albedo == the material's base colour.
    const std::uint8_t* c = pixel(extent.width / 2, extent.height / 2);
    if (!near(c[0], 64) || !near(c[1], 128) || !near(c[2], 191)) {
        return fail("mesh pass self-test: centre pixel is not the material base colour");
    }
    // A corner is outside the triangle: still the clear colour (zero).
    const std::uint8_t* corner = pixel(1, 1);
    if (corner[0] != 0 || corner[1] != 0 || corner[2] != 0 || corner[3] != 0) {
        return fail("mesh pass self-test: corner pixel is not the clear colour");
    }
    return {};
}

std::expected<void, core::Error>
run_specular_aa_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                          rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                          const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{64, 64};
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto pass = MeshPass::create(device, allocator, cache, transfer, cooked_shader_dir);
    if (!pass) return std::unexpected(pass.error());

    // Authored roughness 0: any roughness in the GBuffer material target can only
    // come from the specular-AA normal-variance widening.
    asset::PbrMaterialParams params;
    params.roughness_factor = 0.0F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    // Renders a clip-space triangle with the given vertex normals and returns the
    // centre pixel's stored (widened) roughness from the material target.
    const auto centre_roughness =
        [&](const std::array<glm::vec3, 3>& normals) -> std::expected<std::uint8_t, core::Error> {
        std::array<asset::Vertex, 3> verts{};
        verts[0].position = {-0.5F, -0.5F, 0.0F};
        verts[1].position = {0.5F, -0.5F, 0.0F};
        verts[2].position = {0.0F, 0.5F, 0.0F};
        for (std::size_t i = 0; i < 3; ++i) verts[i].normal = normals[i];

        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        auto vbuf = rhi::upload_device_buffer(allocator, transfer, verts.data(), sizeof(verts),
                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!vbuf) return std::unexpected(vbuf.error());
        auto ibuf = rhi::upload_device_buffer(allocator, transfer, indices.data(), sizeof(indices),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (!ibuf) return std::unexpected(ibuf.error());
        asset::MeshAsset mesh;
        mesh.vertex_buffer = std::move(*vbuf);
        mesh.index_buffer = std::move(*ibuf);
        mesh.vertex_count = 3;
        mesh.index_count = 3;
        mesh.submeshes = {asset::SubMesh{0, 3, 0}};

        auto readback = allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!readback) return std::unexpected(readback.error());
        const VkBuffer rb = readback->handle();

        rhi::TransientImagePool pool(device, allocator);
        rhi::RenderGraph graph(pool);
        const DrawItem item{&mesh, &*material, glm::mat4(1.0F)};
        auto targets = pass->add_to_graph(graph, extent, glm::mat4(1.0F), {&item, 1});
        if (!targets) return std::unexpected(targets.error());
        if (auto compiled = graph.compile(); !compiled) return std::unexpected(compiled.error());

        const VkImage material_image = graph.binding(targets->material).image;
        auto submitted = run_graphics_commands(device, [&](VkCommandBuffer cmd) {
            graph.execute(cmd);
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
            to_src.image = material_image;
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
            vkCmdCopyImageToBuffer(cmd, material_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb,
                                   1, &region);
        });
        if (!submitted) return std::unexpected(submitted.error());

        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const std::size_t o = (static_cast<std::size_t>(extent.height / 2) * extent.width +
                               extent.width / 2) * 4;
        return px[o + 1]; // material target .g = stored roughness
    };

    // Constant normals: zero screen-space variance, roughness must stay 0 (this
    // also guards the pixel-exact lighting/tonemap tests against AA drift).
    auto flat = centre_roughness({glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F),
                                  glm::vec3(0.0F, 0.0F, 1.0F)});
    if (!flat) return std::unexpected(flat.error());
    if (*flat > 2) {
        return fail("specular AA self-test: flat surface picked up roughness");
    }

    // Wildly divergent vertex normals: large per-pixel normal derivatives must
    // widen the stored roughness even though the authored roughness is 0.
    auto curved = centre_roughness({glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(-1.0F, 0.0F, 0.0F),
                                    glm::vec3(0.0F, 1.0F, 0.0F)});
    if (!curved) return std::unexpected(curved.error());
    if (*curved < 20) {
        return fail("specular AA self-test: high-variance normals were not widened");
    }
    return {};
}

} // namespace engine::renderer
