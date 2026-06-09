#include "engine/renderer/exposure_pass.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t      kBins         = 256;
constexpr VkDeviceSize       kHistBytes    = kBins * sizeof(std::uint32_t);

// Log-luminance range covered by the histogram (EV ~[-8, +4]).
constexpr float kMinLogLum = -8.0F;
constexpr float kMaxLogLum = 4.0F;
constexpr float kLogLumRange = kMaxLogLum - kMinLogLum;
constexpr float kTau = 1.1F; // adaptation time constant (seconds)

struct HistogramParams {
    float         min_log_lum;
    float         inv_log_lum_range;
    std::uint32_t width;
    std::uint32_t height;
};
struct AverageParams {
    float min_log_lum;
    float log_lum_range;
    float dt;
    float num_pixels;
    float tau;
    float pad0, pad1, pad2;
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

template <typename Record>
[[nodiscard]] std::expected<void, core::Error> submit_blocking(const rhi::Device& device,
                                                               Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pi, nullptr, &pool) != VK_SUCCESS) {
        return fail("exposure: vkCreateCommandPool failed");
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
                           : fail("exposure: vkQueueSubmit2 failed");
}

void buffer_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize size,
                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkBufferMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.offset = 0;
    b.size = size;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

std::expected<ExposurePass, core::Error>
ExposurePass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    ExposurePass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("exposure: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 2> hist_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
    }};
    const std::array<DescriptorBinding, 2> avg_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
    }};
    auto hist_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, hist_b);
    if (!hist_layout) return std::unexpected(hist_layout.error());
    out.histogram_layout_ = std::move(*hist_layout);
    auto avg_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, avg_b);
    if (!avg_layout) return std::unexpected(avg_layout.error());
    out.average_layout_ = std::move(*avg_layout);

    auto hist = make_pipeline(device, cache, cooked_shader_dir + "/exposure_histogram.spv",
                              out.histogram_layout_, sizeof(HistogramParams));
    if (!hist) return std::unexpected(hist.error());
    out.histogram_pipeline_ = std::move(*hist);
    auto avg = make_pipeline(device, cache, cooked_shader_dir + "/exposure_average.spv",
                             out.average_layout_, sizeof(AverageParams));
    if (!avg) return std::unexpected(avg.error());
    out.average_pipeline_ = std::move(*avg);

    auto histb = allocator.create_buffer(
        kHistBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);
    if (!histb) return std::unexpected(histb.error());
    out.histogram_buffer_ = std::move(*histb);
    out.histogram_address_ = rhi::buffer_device_address(device.handle(), out.histogram_buffer_);

    // Adapted-luminance buffer, host-visible so it starts at 0 (the shader seeds
    // from the first frame's measured luminance when prev <= 0).
    auto expb = allocator.create_buffer(
        sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!expb) return std::unexpected(expb.error());
    const float zero = 0.0F;
    std::memcpy(expb->mapped(), &zero, sizeof(zero));
    out.exposure_buffer_ = std::move(*expb);
    out.exposure_address_ = rhi::buffer_device_address(device.handle(), out.exposure_buffer_);

    return out;
}

std::expected<void, core::Error>
ExposurePass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent,
                           float dt) {
    auto hist_db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, histogram_layout_);
    if (!hist_db) return std::unexpected(hist_db.error());
    histogram_descriptor_ = std::move(*hist_db);
    auto avg_db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, average_layout_);
    if (!avg_db) return std::unexpected(avg_db.error());
    average_descriptor_ = std::move(*avg_db);

    const VkBuffer hist_buf = histogram_buffer_.handle();
    const VkDeviceAddress hist_addr = histogram_address_;
    const VkDeviceAddress exp_addr = exposure_address_;

    HistogramParams hp{kMinLogLum, 1.0F / kLogLumRange, extent.width, extent.height};
    AverageParams ap{kMinLogLum, kLogLumRange, dt,
                     static_cast<float>(extent.width) * static_cast<float>(extent.height), kTau,
                     0.0F, 0.0F, 0.0F};

    graph.add_pass("exposure", rhi::PassType::compute)
        .reads(hdr, rhi::ResourceUsage::sampled_compute)
        .execute([this, hdr, hist_buf, hist_addr, exp_addr, extent, hp, ap](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            histogram_descriptor_.write_sampled_image(0, ctx.resolve(hdr).view, ro);
            histogram_descriptor_.write_storage_buffer(1, hist_addr, kHistBytes);
            average_descriptor_.write_storage_buffer(0, hist_addr, kHistBytes);
            average_descriptor_.write_storage_buffer(1, exp_addr, sizeof(float));

            // Clear the histogram, then build it.
            vkCmdFillBuffer(cmd, hist_buf, 0, kHistBytes, 0);
            buffer_barrier(cmd, hist_buf, kHistBytes, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histogram_pipeline_.handle());
            histogram_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       histogram_pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, histogram_pipeline_.layout(), kComputeStage, 0, sizeof(hp), &hp);
            vkCmdDispatch(cmd, (extent.width + 15) / 16, (extent.height + 15) / 16, 1);

            // Histogram complete -> reduce to the adapted luminance.
            buffer_barrier(cmd, hist_buf, kHistBytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, average_pipeline_.handle());
            average_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     average_pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, average_pipeline_.layout(), kComputeStage, 0, sizeof(ap), &ap);
            vkCmdDispatch(cmd, 1, 1, 1);

            // Make the adapted luminance visible to the tonemap fragment shader.
            buffer_barrier(cmd, exposure_buffer_.handle(), sizeof(float),
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        });
    return {};
}

float ExposurePass::adapted_luminance() const {
    return exposure_buffer_.mapped() != nullptr
               ? *static_cast<const float*>(exposure_buffer_.mapped())
               : 0.0F;
}

std::expected<void, core::Error>
run_exposure_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{64, 64};
    auto exposure = ExposurePass::create(device, allocator, cache, cooked_shader_dir);
    if (!exposure) return std::unexpected(exposure.error());

    auto img = allocator.create_image(VK_FORMAT_R16G16B16A16_SFLOAT, extent,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!img) return std::unexpected(img.error());
    auto view = rhi::create_image_view(device.handle(), img->handle(),
                                       VK_FORMAT_R16G16B16A16_SFLOAT);
    if (!view) return std::unexpected(view.error());
    const VkImage image = img->handle();

    // Fill the image to a uniform luminance and run one fully-adapted exposure
    // pass (huge dt -> adaptation rate ~1). Returns the resulting adapted lum.
    const auto measure = [&](float value, VkImageLayout old_layout)
        -> std::expected<float, core::Error> {
        auto prep = submit_blocking(device, [&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = old_layout;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkClearColorValue c{};
            c.float32[0] = c.float32[1] = c.float32[2] = value;
            c.float32[3] = 1.0F;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);

            b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        if (!prep) return std::unexpected(prep.error());

        rhi::TransientImagePool pool(device, allocator);
        rhi::RenderGraph graph(pool);
        const rhi::ResourceHandle hdr = graph.import_image(
            "exposure_test", image, view->handle(), VK_FORMAT_R16G16B16A16_SFLOAT, extent,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (auto r = exposure->add_to_graph(graph, hdr, extent, 1000.0F); !r) {
            return std::unexpected(r.error());
        }
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());
        auto done = submit_blocking(device, [&](VkCommandBuffer cmd) { graph.execute(cmd); });
        if (!done) return std::unexpected(done.error());
        return exposure->adapted_luminance();
    };

    // Grey 0.25 -> luminance 0.25; the adapted value should track it.
    auto dim = measure(0.25F, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!dim) return std::unexpected(dim.error());
    if (*dim < 0.15F || *dim > 0.45F) {
        return fail("exposure self-test: adapted luminance did not match the dim scene");
    }
    // A bright scene (2.0) should pull the adapted luminance well up.
    auto bright = measure(2.0F, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!bright) return std::unexpected(bright.error());
    if (*bright <= *dim + 0.5F) {
        return fail("exposure self-test: adaptation did not rise for the bright scene");
    }
    return {};
}

} // namespace engine::renderer
