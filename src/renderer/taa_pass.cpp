#include "engine/renderer/taa_pass.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

struct TaaPushConstants {
    glm::mat4 cur_inv_view_proj{1.0F};
    glm::mat4 prev_view_proj{1.0F};
    glm::vec2 extent{0.0F};
    float     alpha = 0.9F;
    float     valid = 0.0F;
};
static_assert(sizeof(TaaPushConstants) == 144, "must match taa.slang Params");

[[nodiscard]] float halton(std::uint64_t i, std::uint32_t base) {
    float f = 1.0F;
    float r = 0.0F;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

template <typename Record>
[[nodiscard]] std::expected<void, core::Error> submit_blocking(const rhi::Device& device,
                                                               Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pi, nullptr, &pool) != VK_SUCCESS) {
        return fail("taa: vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult r = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(device.graphics_queue());
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return r == VK_SUCCESS ? std::expected<void, core::Error>{}
                           : fail("taa: vkQueueSubmit2 failed");
}

} // namespace

glm::vec2 taa_jitter(std::uint64_t index, std::uint32_t width, std::uint32_t height) {
    const float jx = halton(index + 1, 2) - 0.5F;
    const float jy = halton(index + 1, 3) - 0.5F;
    return {jx * 2.0F / static_cast<float>(width), jy * 2.0F / static_cast<float>(height)};
}

std::expected<TaaPass, core::Error>
TaaPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
                const std::string& cooked_shader_dir, VkFormat ldr_format) {
    TaaPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.format_ = ldr_format;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("taa: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 5> b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, b);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/taa.spv");
    if (!shader) return std::unexpected(shader.error());
    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(TaaPushConstants)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = rhi::ComputePipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);
    return out;
}

std::expected<void, core::Error> TaaPass::ensure_history(VkExtent2D extent) {
    if (history_extent_.width == extent.width && history_extent_.height == extent.height &&
        history_[0].handle() != VK_NULL_HANDLE) {
        return {};
    }
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < 2; ++i) {
        auto img = allocator_->create_image(format_, extent, usage);
        if (!img) return std::unexpected(img.error());
        history_[i] = std::move(*img);
        auto view = rhi::create_image_view(device_->handle(), history_[i].handle(), format_);
        if (!view) return std::unexpected(view.error());
        history_view_[i] = std::move(*view);
    }
    // Clear both to black and leave them sampled-ready.
    const VkImage a = history_[0].handle();
    const VkImage bimg = history_[1].handle();
    auto cleared = submit_blocking(*device_, [&](VkCommandBuffer cmd) {
        for (VkImage img : {a, bimg}) {
            VkImageMemoryBarrier2 bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            bar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            bar.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = img;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &bar;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkClearColorValue black{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

            bar.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    });
    if (!cleared) return std::unexpected(cleared.error());
    history_extent_ = extent;
    return {};
}

std::expected<void, core::Error>
TaaPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle current,
                      rhi::ResourceHandle depth, rhi::ResourceHandle swapchain, VkExtent2D extent,
                      const glm::mat4& cur_inv_view_proj, const glm::mat4& prev_view_proj,
                      std::uint64_t frame_index, bool history_valid) {
    if (auto r = ensure_history(extent); !r) return std::unexpected(r.error());

    const std::size_t cur = frame_index % 2;
    const std::size_t prev = (frame_index + 1) % 2;
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    const rhi::ResourceHandle h_cur =
        graph.import_image("taa_history_cur", history_[cur].handle(), history_view_[cur].handle(),
                           format_, extent, ro, ro);
    const rhi::ResourceHandle h_prev =
        graph.import_image("taa_history_prev", history_[prev].handle(),
                           history_view_[prev].handle(), format_, extent, ro, ro);

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    TaaPushConstants push{};
    push.cur_inv_view_proj = cur_inv_view_proj;
    push.prev_view_proj = prev_view_proj;
    push.extent = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    push.alpha = 0.9F;
    push.valid = history_valid ? 1.0F : 0.0F;

    graph.add_pass("taa_resolve", rhi::PassType::compute)
        .reads(current, rhi::ResourceUsage::sampled_compute)
        .reads(h_prev, rhi::ResourceUsage::sampled_compute)
        .reads(depth, rhi::ResourceUsage::sampled_compute)
        .writes(h_cur, rhi::ResourceUsage::storage_write)
        .execute([this, current, h_prev, depth, h_cur, extent, push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_sampled_image(0, ctx.resolve(current).view, ro);
            frame_descriptor_.write_sampled_image(1, ctx.resolve(h_prev).view, ro);
            frame_descriptor_.write_sampled_image(2, ctx.resolve(depth).view, ro);
            frame_descriptor_.write_sampler(3, sampler_.handle());
            frame_descriptor_.write_storage_image(4, ctx.resolve(h_cur).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        });

    // Copy the resolved history to the swapchain for presentation.
    graph.add_pass("taa_present", rhi::PassType::transfer)
        .reads(h_cur, rhi::ResourceUsage::transfer_src)
        .writes(swapchain, rhi::ResourceUsage::transfer_dst)
        .execute([h_cur, swapchain, extent](rhi::PassContext& ctx) {
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {extent.width, extent.height, 1};
            vkCmdCopyImage(ctx.cmd(), ctx.resolve(h_cur).image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx.resolve(swapchain).image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
    return {};
}

std::expected<void, core::Error>
run_taa_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    constexpr VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkExtent2D extent{32, 32};
    auto taa = TaaPass::create(device, allocator, cache, cooked_shader_dir, fmt);
    if (!taa) return std::unexpected(taa.error());

    // Resolve a uniform `value` frame at `frame_index` and return the centre
    // output (0..255). Identity matrices + constant depth -> identity reprojection.
    const auto run = [&](float value, std::uint64_t frame_index,
                         bool valid) -> std::expected<int, core::Error> {
        auto cur = allocator.create_image(fmt, extent,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto depth = allocator.create_image(
            VK_FORMAT_D32_SFLOAT, extent,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto out = allocator.create_image(fmt, extent,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!cur || !depth || !out) return fail("taa self-test: image creation failed");
        auto cur_view = rhi::create_image_view(device.handle(), cur->handle(), fmt);
        auto depth_view = rhi::create_image_view(device.handle(), depth->handle(),
                                                 VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!cur_view || !depth_view) return fail("taa self-test: view creation failed");

        const VkImage cur_img = cur->handle();
        const VkImage depth_img = depth->handle();
        auto prep = submit_blocking(device, [&](VkCommandBuffer cmd) {
            const auto to_dst = [&](VkImage img, VkImageAspectFlags aspect) {
                VkImageMemoryBarrier2 b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img;
                b.subresourceRange = {aspect, 0, 1, 0, 1};
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &dep);
            };
            const auto to_read = [&](VkImage img, VkImageAspectFlags aspect) {
                VkImageMemoryBarrier2 b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img;
                b.subresourceRange = {aspect, 0, 1, 0, 1};
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &dep);
            };
            to_dst(cur_img, VK_IMAGE_ASPECT_COLOR_BIT);
            VkClearColorValue c{};
            c.float32[0] = c.float32[1] = c.float32[2] = value;
            c.float32[3] = 1.0F;
            VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, cur_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &cr);
            to_read(cur_img, VK_IMAGE_ASPECT_COLOR_BIT);

            to_dst(depth_img, VK_IMAGE_ASPECT_DEPTH_BIT);
            VkClearDepthStencilValue dc{0.5F, 0};
            VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd, depth_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dc, 1,
                                        &dr);
            to_read(depth_img, VK_IMAGE_ASPECT_DEPTH_BIT);
        });
        if (!prep) return std::unexpected(prep.error());

        auto out_view = rhi::create_image_view(device.handle(), out->handle(), fmt);
        if (!out_view) return std::unexpected(out_view.error());
        const VkImage out_img = out->handle();

        rhi::TransientImagePool pool(device, allocator);
        rhi::RenderGraph graph(pool);
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const rhi::ResourceHandle h_cur = graph.import_image("taa_test_cur", cur_img,
                                                             cur_view->handle(), fmt, extent, ro, ro);
        const rhi::ResourceHandle h_depth =
            graph.import_image("taa_test_depth", depth_img, depth_view->handle(),
                               VK_FORMAT_D32_SFLOAT, extent, ro, ro);
        const rhi::ResourceHandle h_out =
            graph.import_image("taa_test_out", out_img, out_view->handle(), fmt, extent,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (auto r = taa->add_to_graph(graph, h_cur, h_depth, h_out, extent, glm::mat4(1.0F),
                                       glm::mat4(1.0F), frame_index, valid);
            !r) {
            return std::unexpected(r.error());
        }
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());

        auto readback = allocator.create_buffer(
            static_cast<VkDeviceSize>(extent.width) * extent.height * 4,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!readback) return std::unexpected(readback.error());
        const VkBuffer rb = readback->handle();
        auto done = submit_blocking(device, [&](VkCommandBuffer cmd) {
            graph.execute(cmd);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1,
                                   &region);
        });
        if (!done) return std::unexpected(done.error());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const std::size_t o =
            (static_cast<std::size_t>(extent.height / 2) * extent.width + extent.width / 2) * 4;
        return static_cast<int>(px[o]);
    };

    // Frame 0: no history -> shows the current frame (0.5 -> ~128).
    auto f0 = run(0.5F, 0, false);
    if (!f0) return std::unexpected(f0.error());
    if (*f0 < 118 || *f0 > 138) return fail("taa self-test: first frame is not the current frame");
    // Frame 1: history (0.5) matches current (0.5) -> stays ~128.
    auto f1 = run(0.5F, 1, true);
    if (!f1) return std::unexpected(f1.error());
    if (*f1 < 118 || *f1 > 138) return fail("taa self-test: uniform frame did not resolve to itself");
    // Frame 2: switch to 0.8 with 0.5 history -> the clamp rejects the stale
    // history, so it resolves to ~204 (not a ghosted blend near ~135).
    auto f2 = run(0.8F, 2, true);
    if (!f2) return std::unexpected(f2.error());
    if (*f2 < 188) return fail("taa self-test: stale history ghosted into a flat region");
    return {};
}

} // namespace engine::renderer
