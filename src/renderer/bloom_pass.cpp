#include "engine/renderer/bloom_pass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/lighting_pass.hpp" // hdr_color_format
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t      kMaxMips      = 6;
constexpr std::uint32_t      kMinMipSize   = 8;

struct DownParams {
    float threshold, knee, prefilter, pad;
};
struct UpParams {
    float radius, pad0, pad1, pad2;
};
struct CompositeParams {
    float intensity, radius, pad0, pad1;
};

[[nodiscard]] std::expected<rhi::ComputePipeline, core::Error>
make_pipeline(const rhi::Device& device, rhi::PipelineCache& cache, const std::string& spv,
              const rhi::DescriptorSetLayout& layout, std::uint32_t push_size) {
    auto shader = cache.load(spv);
    if (!shader) return std::unexpected(shader.error());
    const VkDescriptorSetLayout set_layout = layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_size};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    return rhi::ComputePipeline::create(device, cache.handle(), info);
}

[[nodiscard]] std::uint32_t groups(std::uint32_t n) { return (n + 7) / 8; }

[[nodiscard]] float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000U) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1FU;
    const std::uint32_t mant = h & 0x3FFU;
    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant != 0) {
            std::uint32_t e = 127 - 15 + 1;
            std::uint32_t m = mant;
            while ((m & 0x400U) == 0) { m <<= 1; --e; }
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
[[nodiscard]] std::expected<void, core::Error> submit_blocking(const rhi::Device& device,
                                                               Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pi, nullptr, &pool) != VK_SUCCESS) {
        return fail("bloom: vkCreateCommandPool failed");
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
                           : fail("bloom: vkQueueSubmit2 failed");
}

} // namespace

std::expected<BloomPass, core::Error>
BloomPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    BloomPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("bloom: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 3> down_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    const std::array<DescriptorBinding, 4> up_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    auto down_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, down_b);
    if (!down_layout) return std::unexpected(down_layout.error());
    out.down_layout_ = std::move(*down_layout);
    auto up_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, up_b);
    if (!up_layout) return std::unexpected(up_layout.error());
    out.up_layout_ = std::move(*up_layout);

    auto down = make_pipeline(device, cache, cooked_shader_dir + "/bloom_down.spv", out.down_layout_,
                              sizeof(DownParams));
    if (!down) return std::unexpected(down.error());
    out.down_pipeline_ = std::move(*down);
    auto up = make_pipeline(device, cache, cooked_shader_dir + "/bloom_up.spv", out.up_layout_,
                            sizeof(UpParams));
    if (!up) return std::unexpected(up.error());
    out.up_pipeline_ = std::move(*up);
    auto comp = make_pipeline(device, cache, cooked_shader_dir + "/bloom_composite.spv",
                              out.down_layout_, sizeof(CompositeParams));
    if (!comp) return std::unexpected(comp.error());
    out.composite_pipeline_ = std::move(*comp);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);
    return out;
}

std::expected<void, core::Error>
BloomPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent,
                        const BloomParams& params) {
    // Mip chain extents, starting at half resolution.
    std::vector<VkExtent2D> sizes;
    VkExtent2D s{std::max(extent.width / 2, 1u), std::max(extent.height / 2, 1u)};
    for (std::uint32_t i = 0; i < kMaxMips && std::min(s.width, s.height) >= kMinMipSize; ++i) {
        sizes.push_back(s);
        s = {std::max(s.width / 2, 1u), std::max(s.height / 2, 1u)};
    }
    if (sizes.empty()) return {}; // too small to bloom — no-op
    const std::size_t n = sizes.size();

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    std::vector<rhi::ResourceHandle> down(n), up(n);
    for (std::size_t i = 0; i < n; ++i) {
        down[i] = graph.create_transient_image("bloom_down", hdr_color_format, sizes[i], usage);
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        up[i] = graph.create_transient_image("bloom_up", hdr_color_format, sizes[i], usage);
    }

    frame_descriptors_.clear();
    frame_descriptors_.reserve(n + (n > 0 ? n - 1 : 0) + 1);
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkImageLayout gen = VK_IMAGE_LAYOUT_GENERAL;

    const auto add_db = [&](const rhi::DescriptorSetLayout& layout)
        -> std::expected<std::size_t, core::Error> {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout);
        if (!db) return std::unexpected(db.error());
        frame_descriptors_.push_back(std::move(*db));
        return frame_descriptors_.size() - 1;
    };

    // --- downsample chain ---------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        const rhi::ResourceHandle src = (i == 0) ? hdr : down[i - 1];
        const rhi::ResourceHandle dst = down[i];
        const bool prefilter = (i == 0);
        auto idx = add_db(down_layout_);
        if (!idx) return std::unexpected(idx.error());
        const VkExtent2D ds = sizes[i];
        graph.add_pass("bloom_down", rhi::PassType::compute)
            .reads(src, rhi::ResourceUsage::sampled_compute)
            .writes(dst, rhi::ResourceUsage::storage_write)
            .execute([this, di = *idx, src, dst, ds, params, prefilter](rhi::PassContext& ctx) {
                rhi::DescriptorBuffer& db = frame_descriptors_[di];
                db.write_sampled_image(0, ctx.resolve(src).view, ro);
                db.write_sampler(1, sampler_.handle());
                db.write_storage_image(2, ctx.resolve(dst).view, gen);
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_pipeline_.handle());
                db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_pipeline_.layout(), 0);
                DownParams p{params.threshold, params.knee, prefilter ? 1.0F : 0.0F, 0.0F};
                vkCmdPushConstants(cmd, down_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
                vkCmdDispatch(cmd, groups(ds.width), groups(ds.height), 1);
            });
    }

    // --- upsample chain (largest blurred level ends in up[0]) ---------------
    for (std::size_t i = n - 1; i-- > 0;) {
        const rhi::ResourceHandle src = (i + 1 == n - 1) ? down[n - 1] : up[i + 1];
        const rhi::ResourceHandle base = down[i];
        const rhi::ResourceHandle dst = up[i];
        auto idx = add_db(up_layout_);
        if (!idx) return std::unexpected(idx.error());
        const VkExtent2D us = sizes[i];
        graph.add_pass("bloom_up", rhi::PassType::compute)
            .reads(src, rhi::ResourceUsage::sampled_compute)
            .reads(base, rhi::ResourceUsage::sampled_compute)
            .writes(dst, rhi::ResourceUsage::storage_write)
            .execute([this, di = *idx, src, base, dst, us, params](rhi::PassContext& ctx) {
                rhi::DescriptorBuffer& db = frame_descriptors_[di];
                db.write_sampled_image(0, ctx.resolve(src).view, ro);
                db.write_sampler(1, sampler_.handle());
                db.write_sampled_image(2, ctx.resolve(base).view, ro);
                db.write_storage_image(3, ctx.resolve(dst).view, gen);
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, up_pipeline_.handle());
                db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, up_pipeline_.layout(), 0);
                UpParams p{params.radius, 0.0F, 0.0F, 0.0F};
                vkCmdPushConstants(cmd, up_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
                vkCmdDispatch(cmd, groups(us.width), groups(us.height), 1);
            });
    }

    // --- composite back into the full-res HDR -------------------------------
    const rhi::ResourceHandle bloom = (n >= 2) ? up[0] : down[0];
    auto cidx = add_db(down_layout_);
    if (!cidx) return std::unexpected(cidx.error());
    graph.add_pass("bloom_composite", rhi::PassType::compute)
        .reads(bloom, rhi::ResourceUsage::sampled_compute)
        .writes(hdr, rhi::ResourceUsage::storage_write)
        .execute([this, di = *cidx, bloom, hdr, extent, params](rhi::PassContext& ctx) {
            rhi::DescriptorBuffer& db = frame_descriptors_[di];
            db.write_sampled_image(0, ctx.resolve(bloom).view, ro);
            db.write_sampler(1, sampler_.handle());
            db.write_storage_image(2, ctx.resolve(hdr).view, gen);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_.handle());
            db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_.layout(), 0);
            CompositeParams p{params.intensity, params.radius, 0.0F, 0.0F};
            vkCmdPushConstants(cmd, composite_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
            vkCmdDispatch(cmd, groups(extent.width), groups(extent.height), 1);
        });
    return {};
}

std::expected<void, core::Error>
run_bloom_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{128, 128};
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 8;

    auto bloom = BloomPass::create(device, allocator, cache, cooked_shader_dir);
    if (!bloom) return std::unexpected(bloom.error());

    auto hdr = allocator.create_image(hdr_color_format, extent,
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!hdr) return std::unexpected(hdr.error());
    auto hdr_view = rhi::create_image_view(device.handle(), hdr->handle(), hdr_color_format);
    if (!hdr_view) return std::unexpected(hdr_view.error());

    // A 16x16 bright (8.0) block in the centre on an otherwise-black image.
    constexpr std::uint32_t block = 16;
    std::vector<std::uint16_t> px(static_cast<std::size_t>(block) * block * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = px[i + 1] = px[i + 2] = 0x4800; // half 8.0
        px[i + 3] = 0x3C00;                       // half 1.0
    }
    auto staging = allocator.create_staging_buffer(static_cast<VkDeviceSize>(block) * block * 8);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), px.data(), px.size() * sizeof(std::uint16_t));

    const VkImage hdr_image = hdr->handle();
    const VkBuffer stage_buf = staging->handle();
    auto prepared = submit_blocking(device, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = hdr_image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, hdr_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<std::int32_t>(extent.width / 2 - block / 2),
                              static_cast<std::int32_t>(extent.height / 2 - block / 2), 0};
        region.imageExtent = {block, block, 1};
        vkCmdCopyBufferToImage(cmd, stage_buf, hdr_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    if (!prepared) return std::unexpected(prepared.error());

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer rb = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle hdr_handle =
        graph.import_image("bloom_test_hdr", hdr_image, hdr_view->handle(), hdr_color_format, extent,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    BloomParams params;
    params.threshold = 1.0F;
    params.intensity = 1.0F;
    if (auto r = bloom->add_to_graph(graph, hdr_handle, extent, params); !r) {
        return std::unexpected(r.error());
    }
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());

    auto done = submit_blocking(device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, hdr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
    });
    if (!done) return std::unexpected(done.error());

    const auto* halfs = static_cast<const std::uint16_t*>(readback->mapped());
    const auto lum = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t o = (static_cast<std::size_t>(y) * extent.width + x) * 4;
        return half_to_float(halfs[o]) + half_to_float(halfs[o + 1]) + half_to_float(halfs[o + 2]);
    };
    // Centre still bright; a pixel well outside the block now glows (was black).
    if (lum(64, 64) <= 1.0F) {
        return fail("bloom self-test: bright block was lost");
    }
    if (lum(96, 64) <= 0.003F) {
        return fail("bloom self-test: highlight did not bloom into neighbours");
    }
    return {};
}

} // namespace engine::renderer
