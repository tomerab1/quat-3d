#include "engine/renderer/lighting_pass.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "pbr_reference.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

// GPU push-constant block, mirrors LightParams in lighting.slang (192 bytes).
// The light fields match DirectionalLightParams; the camera/light matrices let
// the shader reconstruct world position and sample the shadow map. camera_pos.w
// is the shadows-enabled flag.
struct LightingPushConstants {
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 light_view_proj{1.0F};
    glm::vec4 camera_pos{0.0F};
    glm::vec4 direction{0.0F, 0.0F, 1.0F, 0.0F};
    glm::vec4 color{1.0F};
    glm::vec4 ambient{0.0F};
};
static_assert(sizeof(LightingPushConstants) == 192, "must match lighting.slang LightParams");

// Lighting descriptor set 0: GBuffer albedo/normal/material/depth as sampled
// images (read via Load — no sampler), the HDR output as a storage image, then
// the shadow map (sampled) + its sampler. Matches lighting.slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 7> lighting_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
    }};
}

} // namespace

std::expected<LightingPass, core::Error>
LightingPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    LightingPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("lighting pass: descriptor buffer functions unavailable");
    }

    const auto bindings = lighting_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/lighting.spv");
    if (!shader) return std::unexpected(shader.error());

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(LightingPushConstants)};

    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = rhi::ComputePipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.shadow_sampler_ = std::move(*sampler);

    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
LightingPass::add_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer,
                           VkExtent2D extent, const DirectionalLightParams& light,
                           const glm::mat4& inv_view_proj, const glm::vec3& camera_pos,
                           rhi::ResourceHandle shadow_map, const glm::mat4& light_view_proj) {
    const rhi::ResourceHandle hdr = graph.create_transient_image(
        "hdr_color", hdr_color_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    // When no shadow map is supplied, sample the GBuffer depth as a harmless
    // placeholder at binding 5 (descriptors must be bound) and disable shadows.
    const bool has_shadow = shadow_map.valid();
    const rhi::ResourceHandle shadow = has_shadow ? shadow_map : gbuffer.depth;

    LightingPushConstants push{};
    push.inv_view_proj = inv_view_proj;
    push.light_view_proj = light_view_proj;
    push.camera_pos = glm::vec4(camera_pos, has_shadow ? 1.0F : 0.0F);
    push.direction = light.direction;
    push.color = light.color;
    push.ambient = light.ambient;

    auto pass = graph.add_pass("lighting", rhi::PassType::compute);
    pass.reads(gbuffer.albedo, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.normal, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.material, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.depth, rhi::ResourceUsage::sampled_compute)
        .writes(hdr, rhi::ResourceUsage::storage_write);
    if (has_shadow) {
        pass.reads(shadow_map, rhi::ResourceUsage::sampled_compute);
    }
    pass.execute([this, gbuffer, hdr, shadow, extent, push](rhi::PassContext& ctx) {
        const VkCommandBuffer cmd = ctx.cmd();
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frame_descriptor_.write_sampled_image(0, ctx.resolve(gbuffer.albedo).view, ro);
        frame_descriptor_.write_sampled_image(1, ctx.resolve(gbuffer.normal).view, ro);
        frame_descriptor_.write_sampled_image(2, ctx.resolve(gbuffer.material).view, ro);
        frame_descriptor_.write_sampled_image(3, ctx.resolve(gbuffer.depth).view, ro);
        frame_descriptor_.write_storage_image(4, ctx.resolve(hdr).view, VK_IMAGE_LAYOUT_GENERAL);
        frame_descriptor_.write_sampled_image(5, ctx.resolve(shadow).view, ro);
        frame_descriptor_.write_sampler(6, shadow_sampler_.handle());

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
        frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);

        const std::uint32_t gx = (extent.width + 7) / 8;
        const std::uint32_t gy = (extent.height + 7) / 8;
        vkCmdDispatch(cmd, gx, gy, 1);
    });

    return hdr;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

// IEEE half -> float, sufficient for the normal HDR values this test produces.
[[nodiscard]] float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000U) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1FU;
    const std::uint32_t mant = h & 0x3FFU;
    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant != 0) {
            std::uint32_t e = 127 - 15 + 1;
            std::uint32_t m = mant;
            while ((m & 0x400U) == 0) {
                m <<= 1;
                --e;
            }
            m &= 0x3FFU;
            bits = sign | (e << 23) | (m << 13);
        } else {
            bits = sign;
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out = 0.0F;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

template <typename Record>
[[nodiscard]] std::expected<void, core::Error>
run_graphics_commands(const rhi::Device& device, Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return fail("lighting self-test: vkCreateCommandPool failed");
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
                           : fail("lighting self-test: vkQueueSubmit2 failed");
}

} // namespace

std::expected<void, core::Error>
run_lighting_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                            rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                            const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{64, 64};
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 8;

    // A camera-facing triangle whose normal points at the light.
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

    // A smooth metal: diffuse is killed (metallic = 1), so the lit value is pure
    // Cook-Torrance specular — a strong head-on highlight a Lambert shader could
    // never produce. This makes the test sensitive to the whole BRDF (D/Vis/F).
    // Values chosen to land exactly on 8-bit UNORM steps so GPU and std::round
    // agree (0.3 would hit the 76.5 half-step where the two round differently).
    const glm::vec3 base{0.25F, 0.5F, 0.75F};
    constexpr float test_metallic = 1.0F;  // 255
    constexpr float test_roughness = 0.5F; // 127.5 -> 128 either way
    asset::PbrMaterialParams params;
    params.base_color_factor = {base, 1.0F};
    params.metallic_factor = test_metallic;
    params.roughness_factor = test_roughness;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = MeshPass::create(device, allocator, cache, transfer, cooked_shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting = LightingPass::create(device, allocator, cache, cooked_shader_dir);
    if (!lighting) return std::unexpected(lighting.error());

    // Light along +Z (the triangle's normal), white, intensity 1, no ambient.
    // With the camera also along +Z this is a head-on view: N = V = L = +Z.
    DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 1.0F};
    light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    const DrawItem item{&mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    // Identity view_proj (geometry already in clip space); camera sits at +Z so
    // the reconstructed view vector is +Z, matching the light and normal.
    auto hdr = lighting->add_to_graph(graph, *targets, extent, light, glm::mat4(1.0F),
                                      glm::vec3(0.0F, 0.0F, 1.0F), rhi::ResourceHandle{},
                                      glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());
    if (auto compiled = graph.compile(); !compiled) return std::unexpected(compiled.error());

    const VkImage hdr_image = graph.binding(*hdr).image;
    auto submitted = run_graphics_commands(device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);

        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = hdr_image;
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
        vkCmdCopyImageToBuffer(cmd, hdr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buffer, 1, &region);
    });
    if (!submitted) return std::unexpected(submitted.error());

    const auto* halfs = static_cast<const std::uint16_t*>(readback->mapped());
    const auto rgb = [&](std::uint32_t x, std::uint32_t y, int c) {
        return half_to_float(halfs[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
    };
    const auto near = [](float v, float t) { return std::abs(v - t) <= 0.02F; };
    const auto quant = [](float v) { return std::round(v * 255.0F) / 255.0F; };

    // The GBuffer quantised albedo/metallic/roughness to 8 bits. Reconstruct the
    // centre pixel's view vector exactly as the shader does (identity view_proj,
    // surface depth 0, camera at +Z): low-roughness specular is too sharp to
    // assume perfect head-on for a pixel that is not dead-centre of projection.
    const std::uint32_t cx = extent.width / 2;
    const std::uint32_t cy = extent.height / 2;
    const float u = (static_cast<float>(cx) + 0.5F) / static_cast<float>(extent.width);
    const float vv = (static_cast<float>(cy) + 0.5F) / static_cast<float>(extent.height);
    const glm::vec3 surface(u * 2.0F - 1.0F, vv * 2.0F - 1.0F, 0.0F);
    const glm::vec3 N(0.0F, 0.0F, 1.0F);
    const glm::vec3 V = glm::normalize(glm::vec3(0.0F, 0.0F, 1.0F) - surface);
    const glm::vec3 L(0.0F, 0.0F, 1.0F);
    const glm::vec3 base_q{quant(base.x), quant(base.y), quant(base.z)};
    const glm::vec3 expected = pbr_ref::direct(base_q, quant(test_metallic), quant(test_roughness),
                                               N, V, L, glm::vec3(1.0F), 1.0F);

    if (!near(rgb(cx, cy, 0), expected.x) || !near(rgb(cx, cy, 1), expected.y) ||
        !near(rgb(cx, cy, 2), expected.z)) {
        return fail("lighting self-test: lit centre pixel is not the expected PBR specular");
    }
    // The metallic highlight should be strong (brighter than the albedo), not a
    // dim diffuse term — a guard against a regression that drops the specular lobe.
    if (rgb(cx, cy, 2) < base_q.z) {
        return fail("lighting self-test: metallic specular highlight is too weak");
    }
    if (rgb(1, 1, 0) != 0.0F || rgb(1, 1, 1) != 0.0F || rgb(1, 1, 2) != 0.0F) {
        return fail("lighting self-test: background pixel is not black");
    }
    return {};
}

} // namespace engine::renderer
