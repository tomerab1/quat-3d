// Instanced draws (R9.1): many copies of one mesh through one instanced draw,
// and the per-instance normal matrix under non-uniform scale / mirroring
// (audit A13), rendered offscreen and read back.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/primitives.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using namespace engine;

constexpr VkExtent2D kExtent{128, 128};

// Orthographic camera on +Z looking at the origin, 8x8 world units in view.
glm::mat4 ortho_view_proj() {
    glm::mat4 proj = glm::ortho(-4.0F, 4.0F, -4.0F, 4.0F, 0.1F, 20.0F);
    proj[1][1] *= -1.0F; // Vulkan clip space is y-down
    return proj * glm::lookAt(glm::vec3(0.0F, 0.0F, 5.0F), glm::vec3(0.0F),
                              glm::vec3(0.0F, 1.0F, 0.0F));
}

// Pixel a world position lands on.
glm::uvec2 pixel_of(const glm::mat4& view_proj, const glm::vec3& world) {
    const glm::vec4 clip = view_proj * glm::vec4(world, 1.0F);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return {static_cast<std::uint32_t>((ndc.x * 0.5F + 0.5F) * static_cast<float>(kExtent.width)),
            static_cast<std::uint32_t>((ndc.y * 0.5F + 0.5F) * static_cast<float>(kExtent.height))};
}

// Executes the compiled `graph`, then copies `image` (left by its last pass in
// COLOR_ATTACHMENT_OPTIMAL) into `readback`.
std::expected<void, core::Error> execute_and_read(const rhi::Device& device,
                                                  rhi::RenderGraph& graph, VkImage image,
                                                  VkBuffer readback) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return std::unexpected(core::Error{"instancing test: vkCreateCommandPool failed"});
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
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kExtent.width, kExtent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
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
    if (r != VK_SUCCESS) {
        return std::unexpected(core::Error{"instancing test: vkQueueSubmit2 failed"});
    }
    return {};
}

std::expected<rhi::GpuBuffer, core::Error> make_readback(rhi::GpuAllocator& allocator) {
    return allocator.create_buffer(
        static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

// A unit quad on the plane y + z = 0 (normal (0, 1, 1)/sqrt 2), centred on the
// origin: x in [-0.5, 0.5], (y, z) = (t, -t) for t in [-0.5, 0.5].
scene::MeshData make_tilted_quad() {
    scene::MeshData d;
    const glm::vec3 n = glm::normalize(glm::vec3(0.0F, 1.0F, 1.0F));
    for (const glm::vec2 c : {glm::vec2(-0.5F, -0.5F), glm::vec2(0.5F, -0.5F),
                              glm::vec2(0.5F, 0.5F), glm::vec2(-0.5F, 0.5F)}) {
        asset::Vertex v;
        v.position = {c.x, c.y, -c.y};
        v.normal = n;
        v.uv = c + 0.5F;
        v.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
        d.vertices.push_back(v);
    }
    d.indices = {0, 1, 2, 0, 2, 3};
    d.bounds = {glm::vec3(-0.5F), glm::vec3(0.5F)};
    d.submeshes = {asset::SubMesh{0, 6, 0}};
    return d;
}

// Inverse of oct_encode in gbuffer.slang.
glm::vec3 oct_decode(glm::vec2 e) {
    glm::vec3 n(e.x, e.y, 1.0F - std::abs(e.x) - std::abs(e.y));
    if (n.z < 0.0F) {
        const float x = n.x;
        n.x = (1.0F - std::abs(n.y)) * (x >= 0.0F ? 1.0F : -1.0F);
        n.y = (1.0F - std::abs(x)) * (n.y >= 0.0F ? 1.0F : -1.0F);
    }
    return glm::normalize(n);
}

} // namespace

// Four cubes through one instanced draw (one per submesh) and the full
// deferred chain: each cube's pixel is lit, the gaps between them stay black.
TEST(GpuInstancing, RendersEveryInstanceOfOneInstancedDraw) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;

    auto mesh = scene::upload_mesh(scene::make_cube_data(), g->allocator, g->transfer);
    ASSERT_OK(mesh);
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.8F, 0.8F, 0.8F, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 0.7F;
    auto material =
        asset::upload_material(params, asset::MaterialTextures{}, g->allocator, g->transfer);
    ASSERT_OK(material);

    const std::array<glm::vec3, 4> centres{glm::vec3(-3.0F, -1.0F, 0.0F),
                                           glm::vec3(-1.0F, 1.0F, 0.0F),
                                           glm::vec3(1.0F, -1.0F, 0.0F),
                                           glm::vec3(3.0F, 1.0F, 0.0F)};
    std::vector<renderer::DrawItem> items;
    for (const glm::vec3& c : centres) {
        items.push_back({&*mesh, &*material, glm::translate(glm::mat4(1.0F), c)});
    }
    const renderer::InstancedDrawList list = renderer::batch_draws(items);
    ASSERT_EQ(list.draws.size(), 1U);
    ASSERT_EQ(list.draws[0].instance_count, centres.size());

    auto mesh_pass =
        renderer::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh_pass);
    auto lighting =
        renderer::LightingPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(lighting);
    auto tonemap = renderer::TonemapPass::create(g->device, g->allocator, g->cache, g->shader_dir,
                                                 ldr_format);
    ASSERT_OK(tonemap);
    auto readback = make_readback(g->allocator);
    ASSERT_OK(readback);

    const glm::mat4 view_proj = ortho_view_proj();
    renderer::DirectionalLightParams light;
    light.direction = glm::vec4(glm::normalize(glm::vec3(0.3F, 0.4F, 1.0F)), 0.0F);
    light.color = glm::vec4(1.0F, 1.0F, 1.0F, 3.0F);

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, kExtent, view_proj, list);
    ASSERT_OK(gbuffer);
    auto hdr = lighting->add_to_graph(graph, *gbuffer, kExtent, light, glm::inverse(view_proj),
                                      glm::vec3(0.0F, 0.0F, 5.0F), {});
    ASSERT_OK(hdr);
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "instancing_ldr", ldr_format, kExtent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ASSERT_OK(tonemap->add_to_graph(graph, *hdr, ldr, kExtent));
    ASSERT_OK(graph.compile());
    ASSERT_OK(execute_and_read(g->device, graph, graph.binding(ldr).image, readback->handle()));

    const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
    const auto luma = [&](glm::uvec2 p) {
        const std::size_t o = (static_cast<std::size_t>(p.y) * kExtent.width + p.x) * 4;
        return int(px[o]) + int(px[o + 1]) + int(px[o + 2]);
    };
    for (std::size_t i = 0; i < centres.size(); ++i) {
        EXPECT_GT(luma(pixel_of(view_proj, centres[i])), 60) << "instance " << i << " not lit";
    }
    // Between the cubes (and away from all of them) is background.
    for (const glm::vec3 gap : {glm::vec3(-2.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, 0.0F),
                                glm::vec3(2.0F, 0.0F, 0.0F), glm::vec3(-3.0F, 2.5F, 0.0F)}) {
        EXPECT_LE(luma(pixel_of(view_proj, gap)), 6) << "geometry at (" << gap.x << ", "
                                                     << gap.y << ")";
    }
}

// Audit A13: the GBuffer normal of a tilted quad under non-uniform scale (and
// mirrored non-uniform scale) must be the true surface normal — the inverse
// transpose, not the model 3x3 — and each instance must read its own matrix.
TEST(GpuInstancing, PerInstanceNormalMatrixUnderNonUniformScale) {
    QUAT_REQUIRE_GPU(g);

    auto mesh = scene::upload_mesh(make_tilted_quad(), g->allocator, g->transfer);
    ASSERT_OK(mesh);
    auto mesh_pass =
        renderer::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh_pass);
    auto readback = make_readback(g->allocator);
    ASSERT_OK(readback);

    struct Case {
        glm::vec3 centre;
        glm::vec3 scale;
        glm::vec3 want; // world normal
    };
    const std::array<Case, 3> cases{{
        // Plane y + z = 0 is unchanged.
        {glm::vec3(-2.5F, 0.0F, 0.0F), glm::vec3(1.0F), glm::normalize(glm::vec3(0, 1, 1))},
        // Scaled by (1, 1, 3) it becomes 3y + z = 0.
        {glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 3.0F),
         glm::normalize(glm::vec3(0, 3, 1))},
        // Mirrored in x as well: same plane, normal must not flip.
        {glm::vec3(2.5F, 0.0F, 0.0F), glm::vec3(-1.0F, 1.0F, 3.0F),
         glm::normalize(glm::vec3(0, 3, 1))},
    }};
    std::vector<renderer::DrawItem> items;
    for (const Case& c : cases) {
        items.push_back({&*mesh, nullptr,
                         glm::scale(glm::translate(glm::mat4(1.0F), c.centre), c.scale)});
    }
    const renderer::InstancedDrawList list = renderer::batch_draws(items);
    ASSERT_EQ(list.draws.size(), 1U);

    const glm::mat4 view_proj = ortho_view_proj();
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, kExtent, view_proj, list);
    ASSERT_OK(gbuffer);
    ASSERT_OK(graph.compile());
    ASSERT_OK(execute_and_read(g->device, graph, graph.binding(gbuffer->normal).image,
                               readback->handle()));

    // R16G16_SFLOAT: two halves per pixel.
    const auto* texels = static_cast<const std::uint16_t*>(readback->mapped());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const glm::uvec2 p = pixel_of(view_proj, cases[i].centre);
        const std::size_t o = (static_cast<std::size_t>(p.y) * kExtent.width + p.x) * 2;
        const glm::vec3 n = oct_decode(
            glm::vec2(glm::unpackHalf1x16(texels[o]), glm::unpackHalf1x16(texels[o + 1])));
        EXPECT_GT(glm::dot(n, cases[i].want), 0.999F)
            << "instance " << i << ": normal (" << n.x << ", " << n.y << ", " << n.z
            << "), want (" << cases[i].want.x << ", " << cases[i].want.y << ", "
            << cases[i].want.z << ")";
    }
}
