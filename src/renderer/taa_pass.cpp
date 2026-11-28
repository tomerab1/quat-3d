#include "engine/renderer/taa_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/profile.hpp"
#include "engine/renderer/non_finite_counters.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

// Mirrors Params in taa.slang.
struct TaaPushConstants {
    glm::vec4 render_size{0.0F};  // xy extent, zw 1 / extent
    glm::vec4 display_size{0.0F}; // xy extent, zw 1 / extent
    glm::vec4 jitter{0.0F};       // xy jitter (render px), z sharpness, w history valid
    glm::vec4 exposure{0.0F};     // x read the exposure buffer, y key, z min, w max
    glm::vec4 blend{0.0F};        // x max weight, y sample falloff, z clip rejection, w frame
    std::uint32_t count_non_finite = 0; // 1: the shader counts into g_non_finite
    std::uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(TaaPushConstants) == 96, "must match taa.slang Params");
static_assert(offsetof(TaaPushConstants, count_non_finite) == 80, "must match taa.slang Params");

// Cap on a pixel's accumulated sample weight, from the history feedback f
// (A8's taa.feedback): 1 / (1 - f). With a sample on the pixel every frame the
// current frame then settles at weight 1 - f; the default 0.9 gives classic
// TAA's 0.1. f = 0 shows the current frame only.
constexpr float k_max_feedback = 0.98F;
[[nodiscard]] float max_weight(float feedback) {
    const float f = std::isfinite(feedback) ? std::clamp(feedback, 0.0F, k_max_feedback) : 0.9F;
    return 1.0F / (1.0F - f);
}
// Gaussian falloff per squared display pixel of a sample's weight (its
// confidence and its share of the reconstruction). At 8, a sample one pixel
// away weighs exp(-8) ~ 3e-4, so native TAA keeps a static image pixel-exact.
constexpr float k_sample_falloff = 8.0F;
// Accumulated weight lost per unit (resolve space) the clip moved the history.
constexpr float k_clip_rejection = 8.0F;
// [floats] of the ExposureState the resolve binds.
constexpr VkDeviceSize k_exposure_bytes = 4 * sizeof(float);
[[nodiscard]] float valid_pre_exposure(float pre) {
    return std::isfinite(pre) && pre > 0.0F ? pre : 1.0F;
}

// taa.slang's resources, by name; binding slots come from its reflection.
enum Resource : std::uint8_t {
    r_color,
    r_depth,
    r_velocity,
    r_history,
    r_sampler,
    r_exposure,
    r_history_out,
    r_out,
    r_non_finite,
    r_count
};
constexpr std::array<rhi::BindingRequest, r_count> kResources{{
    {"g_color", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_velocity", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_history", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_linear", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_exposure", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_history_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_non_finite", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

[[nodiscard]] glm::vec4 size_vec(VkExtent2D e) {
    return {static_cast<float>(e.width), static_cast<float>(e.height),
            1.0F / static_cast<float>(e.width), 1.0F / static_cast<float>(e.height)};
}

} // namespace

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
    QUAT_ZONE_N("taa_history_resize"); // only on a new render extent
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
                const std::string& cooked_shader_dir, std::shared_ptr<TaaHistory> history) {
    TaaPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("taa: descriptor buffer functions unavailable");

    auto shader = cache.load(cooked_shader_dir + "/taa.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kComputeStage);
    if (!bindings) return fail("taa: " + bindings.error().message);
    for (const rhi::DescriptorBinding& b : *bindings) out.slots_.push_back(b.binding);
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

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

    // A valid buffer for the exposure slot when the caller has none (the
    // shader does not read it then).
    auto unit = allocator.create_buffer(
        k_exposure_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!unit) return std::unexpected(unit.error());
    constexpr std::array<float, 4> k_unit{1.0F, 1.0F, 0.0F, 0.18F};
    std::memcpy(unit->mapped(), k_unit.data(), sizeof(k_unit));
    if (auto f = unit->flush(); !f) return std::unexpected(f.error());
    out.unit_exposure_ = std::move(*unit);
    out.unit_exposure_address_ = rhi::buffer_device_address(device.handle(), out.unit_exposure_);

    // A valid buffer for the non-finite counter slot when counting is off
    // (the shader does not write it then).
    auto counter = allocator.create_buffer(
        NonFiniteCounters::k_counter_range,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO);
    if (!counter) return std::unexpected(counter.error());
    out.unused_counter_ = std::move(*counter);
    out.unused_counter_address_ =
        rhi::buffer_device_address(device.handle(), out.unused_counter_);

    // A private history assumes the engine's two frames in flight (CLAUDE.md);
    // that only delays freeing replaced images by a frame when fewer are used.
    out.history_ = history ? std::move(history)
                           : std::make_shared<TaaHistory>(device, allocator, 2U);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
TaaPass::add_to_graph(rhi::RenderGraph& graph, const UpscalerInputs& in) {
    if (!in.color.valid() || !in.depth.valid() || !in.velocity.valid()) {
        return fail("taa: colour, depth and velocity inputs are required");
    }
    if (in.render_extent.width == 0 || in.render_extent.height == 0 ||
        in.display_extent.width == 0 || in.display_extent.height == 0) {
        return fail("taa: empty render or display extent");
    }
    TaaHistory& hist = *history_;
    if (auto r = hist.ensure(in.display_extent); !r) return std::unexpected(r.error());
    hist.collect_retired();
    if (in.reset) hist.invalidate();

    // Ping-pong on the history's global frame counter: this frame writes
    // images_[cur] and reads images_[prev], which the previous frame wrote —
    // regardless of which frame-in-flight slot recorded it (audit A2).
    TaaHistory::Image& hc = hist.images_[hist.frame_ % 2];
    TaaHistory::Image& hp = hist.images_[(hist.frame_ + 1) % 2];
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkExtent2D display = in.display_extent;

    // Import each image in the state the previous frame left it, with that
    // frame's access as the "available" scope: the graph's first barrier then
    // waits on it (same queue, earlier submission). Final layout UNDEFINED =
    // no trailing transition; the state is recorded below instead.
    const rhi::ResourceHandle h_cur =
        graph.import_image("taa_history_cur", hc.image.handle(), hc.view.handle(),
                           TaaHistory::k_format, display, hc.layout, VK_IMAGE_LAYOUT_UNDEFINED,
                           hc.stage, hc.access);
    const rhi::ResourceHandle h_prev =
        graph.import_image("taa_history_prev", hp.image.handle(), hp.view.handle(),
                           TaaHistory::k_format, display, hp.layout, VK_IMAGE_LAYOUT_UNDEFINED,
                           hp.stage, hp.access);
    const rhi::ResourceHandle h_out = graph.create_transient_image(
        "upscaled_hdr", k_output_format, display,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    TaaPushConstants push{};
    push.render_size = size_vec(in.render_extent);
    push.display_size = size_vec(display);
    push.jitter = glm::vec4(in.jitter.pixels, glm::clamp(in.sharpness, 0.0F, 1.0F),
                            hist.valid_ ? 1.0F : 0.0F);
    // The resolve weights samples in exposed space: the auto exposure of
    // the true luminance over this frame's pre-exposure (~1). The history
    // was pre-exposed with the previous frame's multiplier: rescale it.
    const float pre = valid_pre_exposure(in.pre_exposure);
    const float history_scale = hist.valid_ ? pre / valid_pre_exposure(hist.pre_exposure_) : 1.0F;
    push.exposure = glm::vec4(in.exposure != 0 ? 1.0F : 0.0F, 1.0F / pre,
                              std::isfinite(history_scale) ? history_scale : 1.0F, 0.0F);
    push.blend = glm::vec4(max_weight(in.feedback), k_sample_falloff, k_clip_rejection,
                           static_cast<float>(hist.frame_ % (1U << 24))); // dither seed
    push.count_non_finite = in.non_finite_counter != 0 ? 1U : 0U;
    const VkDeviceAddress non_finite =
        in.non_finite_counter != 0 ? in.non_finite_counter : unused_counter_address_;
    const VkDeviceAddress exposure = in.exposure != 0 ? in.exposure : unit_exposure_address_;
    const bool exposure_from_gpu = in.exposure != 0;

    const rhi::ResourceHandle color = in.color;
    const rhi::ResourceHandle depth = in.depth;
    const rhi::ResourceHandle velocity = in.velocity;
    graph.add_pass("taa_resolve", rhi::PassType::compute)
        .reads(color, rhi::ResourceUsage::sampled_compute)
        .reads(depth, rhi::ResourceUsage::sampled_compute)
        .reads(velocity, rhi::ResourceUsage::sampled_compute)
        .reads(h_prev, rhi::ResourceUsage::sampled_compute)
        .writes(h_cur, rhi::ResourceUsage::storage_write)
        .writes(h_out, rhi::ResourceUsage::storage_write)
        .execute([this, color, depth, velocity, h_prev, h_cur, h_out, display, push, exposure,
                  exposure_from_gpu, non_finite](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            if (exposure_from_gpu) {
                // The adapted luminance was written by the previous frame's
                // exposure pass (compute), whose trailing barrier only
                // covers the tonemap's fragment reads.
                VkMemoryBarrier2 mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd, &dep);
            }
            const std::vector<std::uint32_t>& s = slots_;
            frame_descriptor_.write_sampled_image(s[r_color], ctx.resolve(color).view, ro);
            frame_descriptor_.write_sampled_image(s[r_depth], ctx.resolve(depth).view, ro);
            frame_descriptor_.write_sampled_image(s[r_velocity], ctx.resolve(velocity).view, ro);
            frame_descriptor_.write_sampled_image(s[r_history], ctx.resolve(h_prev).view, ro);
            frame_descriptor_.write_sampler(s[r_sampler], sampler_.handle());
            frame_descriptor_.write_storage_buffer(s[r_exposure], exposure, k_exposure_bytes);
            frame_descriptor_.write_storage_image(s[r_history_out], ctx.resolve(h_cur).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            frame_descriptor_.write_storage_image(s[r_out], ctx.resolve(h_out).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            frame_descriptor_.write_storage_buffer(s[r_non_finite], non_finite,
                                                   NonFiniteCounters::k_counter_range);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (display.width + 7) / 8, (display.height + 7) / 8, 1);
            if (push.count_non_finite != 0) NonFiniteCounters::make_host_visible(cmd);
        });

    // What this frame leaves the pair in, for the next frame's import.
    hc.layout = VK_IMAGE_LAYOUT_GENERAL;
    hc.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    hc.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    hp.layout = ro;
    hp.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    hp.access = 0; // read only: the next writer needs an execution dependency only
    hist.valid_ = true;
    hist.pre_exposure_ = pre;
    ++hist.frame_;
    return h_out;
}

// ---------------------------------------------------------------------------
// TaaUpscaler
// ---------------------------------------------------------------------------

std::expected<std::unique_ptr<TaaUpscaler>, core::Error>
TaaUpscaler::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                    std::uint32_t frames_in_flight) {
    auto out = std::make_unique<TaaUpscaler>();
    const std::uint32_t slots = frames_in_flight == 0 ? 1 : frames_in_flight;
    out->history_ = std::make_shared<TaaHistory>(device, allocator, slots);
    for (std::uint32_t i = 0; i < slots; ++i) {
        auto pass = TaaPass::create(device, allocator, cache, cooked_shader_dir, out->history_);
        if (!pass) return std::unexpected(pass.error());
        out->passes_.push_back(std::move(*pass));
    }
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
TaaUpscaler::add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot, const UpscalerInputs& in) {
    if (slot >= passes_.size()) return fail("taa upscaler: frame slot out of range");
    return passes_[slot].add_to_graph(graph, in);
}

} // namespace engine::renderer
