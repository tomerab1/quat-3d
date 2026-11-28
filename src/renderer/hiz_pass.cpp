#include "engine/renderer/hiz_pass.hpp"

#include <algorithm>
#include <array>
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
constexpr std::uint32_t      kGroupSize = 8; // hiz_build.slang numthreads
constexpr std::uint32_t      kFloatInfBits = 0x7F800000U;

// Mirrors HiZParams in hiz_build.slang.
struct HiZPush {
    std::uint32_t src_w = 0;
    std::uint32_t src_h = 0;
    std::uint32_t dst_w = 0;
    std::uint32_t dst_h = 0;
    std::uint32_t first = 0;
};

constexpr std::array<rhi::BindingRequest, 5> kResources{{
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_level0", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_src", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_dst", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_bounds", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

void compute_to_compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

std::expected<HiZPass, core::Error> HiZPass::create(const rhi::Device& device,
                                                    rhi::GpuAllocator& allocator,
                                                    rhi::PipelineCache& cache,
                                                    const std::string& cooked_shader_dir) {
    // RG32F storage images need the extended storage formats (Device).
    const std::array<VkFormat, 1> formats{hiz_format};
    if (auto ok = device.require_extended_storage_formats(formats, "hi-z pass"); !ok) {
        return std::unexpected(ok.error());
    }
    HiZPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("hi-z pass: descriptor buffer functions unavailable");

    auto shader = cache.load(cooked_shader_dir + "/hiz_build.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kComputeStage);
    if (!bindings) return fail("hi-z pass: " + bindings.error().message);
    const auto& b = *bindings;
    out.slots_ = {b[0].binding, b[1].binding, b[2].binding, b[3].binding, b[4].binding};
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(HiZPush)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    // Host-visible so tests and tools can read the bounds back; the GPU
    // writes it with atomics each frame.
    auto bounds = allocator.create_buffer(
        k_bounds_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!bounds) return std::unexpected(bounds.error());
    out.bounds_ = std::move(*bounds);
    out.bounds_address_ = rhi::buffer_device_address(device.handle(), out.bounds_);
    return out;
}

std::expected<void, core::Error> HiZPass::ensure(VkExtent2D extent) {
    if (image_.handle() != VK_NULL_HANDLE && extent_.width == extent.width &&
        extent_.height == extent.height) {
        return {};
    }
    // The pass belongs to one frame slot whose previous frame has finished
    // (fence-waited), so the old pyramid can go right away.
    descriptors_.clear();
    mip_views_.clear();
    full_view_ = {};
    image_ = {};

    const std::uint32_t mips = rhi::full_mip_levels(extent);
    auto image = allocator_->create_image(hiz_format, extent,
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                          mips);
    if (!image) return std::unexpected(image.error());
    image_ = std::move(*image);
    const VkDevice vk = device_->handle();
    auto full = rhi::create_image_view(vk, image_.handle(), hiz_format, VK_IMAGE_ASPECT_COLOR_BIT,
                                       mips);
    if (!full) return std::unexpected(full.error());
    full_view_ = std::move(*full);

    mip_extents_.clear();
    VkExtent2D level = extent;
    for (std::uint32_t m = 0; m < mips; ++m) {
        auto view = rhi::create_image_view(vk, image_.handle(), hiz_format,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_2D, m);
        if (!view) return std::unexpected(view.error());
        mip_views_.push_back(std::move(*view));
        mip_extents_.push_back(level);
        level = {std::max(level.width / 2, 1U), std::max(level.height / 2, 1U)};
    }
    // Dispatch i reads mip i (the depth buffer for i = 0, which also writes
    // mip 0) and writes mip i + 1; a single-level pyramid only copies.
    const std::uint32_t dispatches = std::max(mips, 2U) - 1;
    for (std::uint32_t i = 0; i < dispatches; ++i) {
        auto descriptor = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
        if (!descriptor) return std::unexpected(descriptor.error());
        rhi::DescriptorBuffer& d = descriptors_.emplace_back(std::move(*descriptor));
        const std::uint32_t dst = std::min(i + 1, mips - 1);
        d.write_storage_image(slots_.level0, mip_views_[0].handle(), VK_IMAGE_LAYOUT_GENERAL);
        d.write_storage_image(slots_.src, mip_views_[i].handle(), VK_IMAGE_LAYOUT_GENERAL);
        d.write_storage_image(slots_.dst, mip_views_[dst].handle(), VK_IMAGE_LAYOUT_GENERAL);
        d.write_storage_buffer(slots_.bounds, bounds_address_, k_bounds_bytes);
    }
    extent_ = extent;
    return {};
}

std::expected<HiZTargets, core::Error>
HiZPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle depth, VkExtent2D extent) {
    if (device_ == nullptr) return fail("hi-z pass: not created");
    if (!depth.valid() || extent.width == 0 || extent.height == 0) {
        return fail("hi-z pass: needs a depth buffer and a non-empty extent");
    }
    if (auto e = ensure(extent); !e) return std::unexpected(e.error());

    HiZTargets out;
    out.mip_count = static_cast<std::uint32_t>(mip_views_.size());
    out.extent = extent;
    out.depth_bounds_address = bounds_address_;
    out.pyramid = graph.import_image("hiz", image_.handle(), full_view_.handle(), hiz_format,
                                     extent, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    out.depth_bounds = graph.import_buffer("depth_bounds", bounds_.handle(), k_bounds_bytes);

    const rhi::ResourceHandle bounds = out.depth_bounds;
    (void)graph.add_pass("hiz_bounds_reset", rhi::PassType::transfer)
        .writes(bounds, rhi::ResourceUsage::transfer_dst)
        .execute([bounds](rhi::PassContext& ctx) {
            const VkBuffer b = ctx.resolve(bounds).buffer;
            vkCmdFillBuffer(ctx.cmd(), b, 0, sizeof(std::uint32_t), kFloatInfBits);
            vkCmdFillBuffer(ctx.cmd(), b, sizeof(std::uint32_t), sizeof(std::uint32_t), 0);
        });

    const rhi::ResourceHandle pyramid = out.pyramid;
    (void)graph.add_pass("hiz_build", rhi::PassType::compute)
        .reads(depth, rhi::ResourceUsage::sampled_compute)
        .writes(bounds, rhi::ResourceUsage::buffer_write_compute)
        .writes(pyramid, rhi::ResourceUsage::storage_write)
        .execute([this, depth](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            descriptors_[0].write_sampled_image(slots_.depth, ctx.resolve(depth).view,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            for (std::size_t i = 1; i < descriptors_.size(); ++i) {
                // Bound but only read by the first dispatch.
                descriptors_[i].write_sampled_image(slots_.depth, ctx.resolve(depth).view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            for (std::size_t i = 0; i < descriptors_.size(); ++i) {
                if (i > 0) compute_to_compute_barrier(cmd);
                const VkExtent2D src = mip_extents_[i];
                const VkExtent2D dst = mip_extents_[std::min(i + 1, mip_extents_.size() - 1)];
                const HiZPush push{src.width, src.height, dst.width, dst.height,
                                   i == 0 ? 1U : 0U};
                descriptors_[i].bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
                vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push),
                                   &push);
                vkCmdDispatch(cmd, (dst.width + kGroupSize - 1) / kGroupSize,
                              (dst.height + kGroupSize - 1) / kGroupSize, 1);
            }
        });
    return out;
}

} // namespace engine::renderer
