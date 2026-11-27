#include "engine/renderer/taa_pass.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    float     swap_rb = 0.0F;
    float     pad0 = 0.0F;
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

} // namespace

glm::vec2 taa_jitter(std::uint64_t index, std::uint32_t width, std::uint32_t height) {
    const float jx = halton(index + 1, 2) - 0.5F;
    const float jy = halton(index + 1, 3) - 0.5F;
    return {jx * 2.0F / static_cast<float>(width), jy * 2.0F / static_cast<float>(height)};
}

// ---------------------------------------------------------------------------
// TaaHistory
// ---------------------------------------------------------------------------

TaaHistory::TaaHistory(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       std::uint32_t frames_in_flight)
    : device_(&device), allocator_(&allocator),
      frames_in_flight_(frames_in_flight == 0 ? 1 : frames_in_flight) {}

std::expected<void, core::Error> TaaHistory::ensure(VkExtent2D extent) {
    if (extent_.width == extent.width && extent_.height == extent.height &&
        images_[0].image.handle() != VK_NULL_HANDLE) {
        return {};
    }
    // The other frame in flight may still read/write the old pair: retire it
    // instead of destroying it here (collect_retired frees it later).
    if (images_[0].image.handle() != VK_NULL_HANDLE) {
        Retired old;
        old.images = std::move(images_);
        old.last_frame = frame_ == 0 ? 0 : frame_ - 1;
        retired_.push_back(std::move(old));
    }
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (Image& slot : images_) {
        auto img = allocator_->create_image(k_format, extent, usage);
        if (!img) return std::unexpected(img.error());
        auto view = rhi::create_image_view(device_->handle(), img->handle(), k_format);
        if (!view) return std::unexpected(view.error());
        // Contents are undefined; history is invalid until the first resolve
        // writes it, so no clear (and no queue stall) is needed.
        slot = Image{std::move(*img), std::move(*view), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_NONE, 0};
    }
    extent_ = extent;
    valid_ = false;
    return {};
}

void TaaHistory::collect_retired() {
    // Resolve `frame_` is recorded after waiting on the fence of the slot that
    // recorded resolve `frame_ - frames_in_flight_`, so everything up to that
    // one has finished on the GPU.
    std::erase_if(retired_, [this](const Retired& r) {
        return frame_ >= r.last_frame + frames_in_flight_;
    });
}

// ---------------------------------------------------------------------------
// TaaPass
// ---------------------------------------------------------------------------

std::expected<TaaPass, core::Error>
TaaPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
                const std::string& cooked_shader_dir, VkFormat ldr_format,
                std::shared_ptr<TaaHistory> history) {
    TaaPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    // The resolve writes an R8G8B8A8 storage image (always storage-capable,
    // unlike B8G8R8A8 — audit A10) whose bytes are copied verbatim into the
    // target; for a BGRA target the shader stores the channels swapped.
    switch (ldr_format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        out.swap_rb_ = false;
        break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        out.swap_rb_ = true;
        break;
    default:
        return fail("taa: unsupported LDR/target format " +
                    std::to_string(static_cast<int>(ldr_format)));
    }

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("taa: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 6> b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
        {0, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
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
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // A private history assumes the engine's two frames in flight (CLAUDE.md);
    // that only delays freeing replaced images by a frame when fewer are used.
    out.history_ = history ? std::move(history)
                           : std::make_shared<TaaHistory>(device, allocator, 2U);
    return out;
}

std::expected<void, core::Error>
TaaPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle current,
                      rhi::ResourceHandle depth, rhi::ResourceHandle target, VkExtent2D extent,
                      const glm::mat4& view_proj) {
    TaaHistory& hist = *history_;
    if (auto r = hist.ensure(extent); !r) return std::unexpected(r.error());
    hist.collect_retired();

    // Ping-pong on the history's global frame counter: this frame writes
    // images_[cur] and reads images_[prev], which the previous frame wrote —
    // regardless of which frame-in-flight slot recorded it (audit A2).
    TaaHistory::Image& hc = hist.images_[hist.frame_ % 2];
    TaaHistory::Image& hp = hist.images_[(hist.frame_ + 1) % 2];
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Import each image in the state the previous frame left it, with that
    // frame's access as the "available" scope: the graph's first barrier then
    // waits on it (same queue, earlier submission). Final layout UNDEFINED =
    // no trailing transition; the state is recorded below instead.
    const rhi::ResourceHandle h_cur =
        graph.import_image("taa_history_cur", hc.image.handle(), hc.view.handle(),
                           TaaHistory::k_format, extent, hc.layout, VK_IMAGE_LAYOUT_UNDEFINED,
                           hc.stage, hc.access);
    const rhi::ResourceHandle h_prev =
        graph.import_image("taa_history_prev", hp.image.handle(), hp.view.handle(),
                           TaaHistory::k_format, extent, hp.layout, VK_IMAGE_LAYOUT_UNDEFINED,
                           hp.stage, hp.access);
    const rhi::ResourceHandle h_out = graph.create_transient_image(
        "taa_output", VK_FORMAT_R8G8B8A8_UNORM, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    // Reprojection between UNjittered matrices (audit A14): with a static
    // camera every pixel maps onto its own centre in the history.
    TaaPushConstants push{};
    push.cur_inv_view_proj = glm::inverse(view_proj);
    push.prev_view_proj = hist.valid_ ? hist.prev_view_proj_ : view_proj;
    push.swap_rb = swap_rb_ ? 1.0F : 0.0F;
    push.alpha = 0.9F;
    push.valid = hist.valid_ ? 1.0F : 0.0F;

    graph.add_pass("taa_resolve", rhi::PassType::compute)
        .reads(current, rhi::ResourceUsage::sampled_compute)
        .reads(h_prev, rhi::ResourceUsage::sampled_compute)
        .reads(depth, rhi::ResourceUsage::sampled_compute)
        .writes(h_cur, rhi::ResourceUsage::storage_write)
        .writes(h_out, rhi::ResourceUsage::storage_write)
        .execute([this, current, h_prev, depth, h_cur, h_out, extent,
                  push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_sampled_image(0, ctx.resolve(current).view, ro);
            frame_descriptor_.write_sampled_image(1, ctx.resolve(h_prev).view, ro);
            frame_descriptor_.write_sampled_image(2, ctx.resolve(depth).view, ro);
            frame_descriptor_.write_sampler(3, sampler_.handle());
            frame_descriptor_.write_storage_image(4, ctx.resolve(h_cur).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            frame_descriptor_.write_storage_image(5, ctx.resolve(h_out).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        });

    // Copy the resolved result into the target (byte copy between
    // size-compatible 32-bit formats; channel order was fixed in the shader).
    graph.add_pass("taa_present", rhi::PassType::transfer)
        .reads(h_out, rhi::ResourceUsage::transfer_src)
        .writes(target, rhi::ResourceUsage::transfer_dst)
        .execute([h_out, target, extent](rhi::PassContext& ctx) {
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {extent.width, extent.height, 1};
            vkCmdCopyImage(ctx.cmd(), ctx.resolve(h_out).image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx.resolve(target).image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });

    // What this frame leaves the pair in, for the next frame's import.
    hc.layout = VK_IMAGE_LAYOUT_GENERAL;
    hc.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    hc.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    hp.layout = ro;
    hp.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    hp.access = 0; // read only: the next writer needs an execution dependency only
    hist.prev_view_proj_ = view_proj;
    hist.valid_ = true;
    ++hist.frame_;
    return {};
}
} // namespace engine::renderer
