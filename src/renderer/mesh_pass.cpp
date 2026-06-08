#include "engine/renderer/mesh_pass.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kMaterialStages = VK_SHADER_STAGE_FRAGMENT_BIT;

// Material descriptor set 0, matching PbrMaterial in material.slang (verified
// against the gbuffer.slang SPIR-V): uniform params at 0, the five optional
// textures as sampled images at 1..5, and the shared sampler at 6.
[[nodiscard]] std::array<rhi::DescriptorBinding, 7> material_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kMaterialStages},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kMaterialStages},
    }};
}

} // namespace

std::expected<MeshPass, core::Error>
MeshPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                 rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                 const std::string& cooked_shader_dir) {
    MeshPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("mesh pass: descriptor buffer functions unavailable");
    }

    const auto bindings = material_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.material_layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/gbuffer.spv");
    if (!shader) return std::unexpected(shader.error());

    // Vertex input mirrors asset::Vertex.
    const VkVertexInputBindingDescription vbind{0, sizeof(asset::Vertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 4> vattrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(asset::Vertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(asset::Vertex, tangent)},
    }};
    const std::array<VkFormat, 3> color_formats{gbuffer_albedo_format, gbuffer_normal_format,
                                                gbuffer_material_format};
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(GBufferPushConstants)};
    const VkDescriptorSetLayout set_layout = out.material_layout_.handle();

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = color_formats;
    info.depth_format = gbuffer_depth_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    info.cull_mode = VK_CULL_MODE_NONE;                  // double-sided for now
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};

    auto pipeline = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle());
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // 1x1 opaque-white fallback for unbound texture slots.
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    auto img = rhi::upload_device_image(allocator, transfer, white.data(), white.size(),
                                        VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM);
    if (!img) return std::unexpected(img.error());
    auto view = rhi::create_image_view(device.handle(), img->handle(), VK_FORMAT_R8G8B8A8_UNORM);
    if (!view) return std::unexpected(view.error());
    out.fallback_texture_.image = std::move(*img);
    out.fallback_texture_.view = std::move(*view);
    out.fallback_texture_.width = 1;
    out.fallback_texture_.height = 1;
    out.fallback_texture_.format = VK_FORMAT_R8G8B8A8_UNORM;

    auto mat = asset::upload_material(asset::PbrMaterialParams{}, asset::MaterialTextures{},
                                      allocator, transfer);
    if (!mat) return std::unexpected(mat.error());
    out.default_material_ = std::move(*mat);

    return out;
}

VkImageView MeshPass::texture_view_or_fallback(
    const asset::AssetHandle<asset::TextureAsset>& handle) const {
    if (handle.valid() && handle.is_loaded() && handle->valid()) {
        return handle->view.handle();
    }
    return fallback_texture_.view.handle();
}

std::expected<GBufferTargets, core::Error>
MeshPass::add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                       std::span<const DrawItem> draws) {
    frame_descriptors_.clear();
    frame_draws_.clear();
    frame_descriptors_.reserve(draws.size());
    frame_draws_.reserve(draws.size());

    // GBuffer targets. SAMPLED is added so the lighting pass (3.6) can read them;
    // TRANSFER_SRC lets the self-test (and debug tooling) read them back.
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                          | VK_IMAGE_USAGE_SAMPLED_BIT
                                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    GBufferTargets targets;
    targets.albedo =
        graph.create_transient_image("gbuffer_albedo", gbuffer_albedo_format, extent, color_usage);
    targets.normal =
        graph.create_transient_image("gbuffer_normal", gbuffer_normal_format, extent, color_usage);
    targets.material = graph.create_transient_image("gbuffer_material", gbuffer_material_format,
                                                    extent, color_usage);
    targets.depth = graph.create_transient_image(
        "gbuffer_depth", gbuffer_depth_format, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    for (const DrawItem& d : draws) {
        if (d.mesh == nullptr || !d.mesh->valid()) continue;
        const asset::MaterialAsset& mat =
            (d.material != nullptr && d.material->valid()) ? *d.material : default_material_;

        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, material_layout_);
        if (!db) return std::unexpected(db.error());
        db->write_uniform_buffer(0, mat.param_address, sizeof(asset::PbrMaterialParams));
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        db->write_sampled_image(1, texture_view_or_fallback(mat.textures.base_color), ro);
        db->write_sampled_image(2, texture_view_or_fallback(mat.textures.normal), ro);
        db->write_sampled_image(3, texture_view_or_fallback(mat.textures.metallic_roughness), ro);
        db->write_sampled_image(4, texture_view_or_fallback(mat.textures.emissive), ro);
        db->write_sampled_image(5, texture_view_or_fallback(mat.textures.occlusion), ro);
        db->write_sampler(6, sampler_.handle());

        FrameDraw fd;
        fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                                : d.mesh->vertex_buffer.handle();
        fd.index_buffer = d.mesh->index_buffer.handle();
        fd.submeshes = d.mesh->submeshes;
        fd.model = d.model;
        fd.descriptor_index = static_cast<std::uint32_t>(frame_descriptors_.size());
        frame_descriptors_.push_back(std::move(*db));
        frame_draws_.push_back(fd);
    }

    const GBufferTargets t = targets;
    const glm::mat4 vp = view_proj;
    graph.add_pass("gbuffer", rhi::PassType::graphics)
        .writes(t.albedo, rhi::ResourceUsage::color_attachment)
        .writes(t.normal, rhi::ResourceUsage::color_attachment)
        .writes(t.material, rhi::ResourceUsage::color_attachment)
        .writes(t.depth, rhi::ResourceUsage::depth_attachment)
        .execute([this, t, extent, vp](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const rhi::ResourceBinding a = ctx.resolve(t.albedo);
            const rhi::ResourceBinding n = ctx.resolve(t.normal);
            const rhi::ResourceBinding m = ctx.resolve(t.material);
            const rhi::ResourceBinding depth = ctx.resolve(t.depth);

            const auto color_attachment = [](VkImageView view) {
                VkRenderingAttachmentInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ai.imageView = view;
                ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ai.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                ai.clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
                return ai;
            };
            const std::array<VkRenderingAttachmentInfo, 3> colors{
                color_attachment(a.view), color_attachment(n.view), color_attachment(m.view)};

            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = depth.view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.clearValue.depthStencil = {1.0F, 0};

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = &depth_attachment;

            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

            for (const FrameDraw& fd : frame_draws_) {
                frame_descriptors_[fd.descriptor_index].bind(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0);

                GBufferPushConstants push{};
                push.view_proj = vp;
                push.model = fd.model;
                vkCmdPushConstants(cmd, pipeline_.layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);

                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
                vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);

                for (const asset::SubMesh& sm : fd.submeshes) {
                    vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_offset, 0, 0);
                }
            }

            vkCmdEndRendering(cmd);
        });

    return targets;
}

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

} // namespace engine::renderer
