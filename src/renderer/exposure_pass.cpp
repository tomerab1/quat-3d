#include "engine/renderer/exposure_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "engine/renderer/photometry.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t      kBins         = photometry::k_histogram_bins;
constexpr VkDeviceSize       kHistBytes    = kBins * sizeof(std::uint32_t);
constexpr VkDeviceSize       kStateBytes   = ExposureState::k_floats * sizeof(float);

constexpr float kMinLogLum = photometry::k_histogram_min_log2;
constexpr float kLogLumRange = photometry::k_histogram_max_log2 - photometry::k_histogram_min_log2;

// Mirror the push constants of exposure_histogram.slang / exposure_average.slang.
struct HistogramParams {
    float         min_log_lum;
    float         inv_log_lum_range;
    std::uint32_t width;
    std::uint32_t height;
    float         inv_pre_exposure;
    float         center_weight;
    float         pad0, pad1;
};
static_assert(sizeof(HistogramParams) == 32);
struct AverageParams {
    float min_log_lum;
    float log_lum_range;
    float dt;
    float tau;
    float low_percent;
    float high_percent;
    float key;
    float adaptive_key;
    float min_exposure;
    float max_exposure;
    float nits_per_unit;
    float key_reference;
    float log_inv_pre_exposure;
    float reset;
    float pad0, pad1;
};
static_assert(sizeof(AverageParams) == 64);

[[nodiscard]] float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

[[nodiscard]] std::expected<rhi::ManagedPipeline, core::Error>
make_pipeline(rhi::PipelineCache& cache, const std::string& spv,
              const rhi::DescriptorSetLayout& layout, std::uint32_t push_size) {
    auto shader = cache.load(spv);
    if (!shader) return std::unexpected(shader.error());
    const VkDescriptorSetLayout set_layout = layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_size};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    return cache.create_compute_pipeline(info);
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

std::expected<std::shared_ptr<ExposureState>, core::Error>
ExposureState::create(const rhi::Device& device, rhi::GpuAllocator& allocator) {
    // Host-visible so it starts at 0 (the shader seeds from the first frame's
    // measured luminance when prev <= 0) and can be read back for inspection.
    auto buffer = allocator.create_buffer(
        kStateBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buffer) return std::unexpected(buffer.error());
    // Not seeded; exposure 1 until the first frame is measured.
    const std::array<float, ExposureState::k_floats> init{0.0F, 1.0F, 0.0F, 0.18F};
    std::memcpy(buffer->mapped(), init.data(), sizeof(init));
    if (auto flushed = buffer->flush(); !flushed) return std::unexpected(flushed.error());
    auto state = std::make_shared<ExposureState>(Key{});
    state->buffer_ = std::move(*buffer);
    state->address_ = rhi::buffer_device_address(device.handle(), state->buffer_);
    return state;
}

namespace {
// Element `i` of the mapped state, or `fallback`.
[[nodiscard]] float read_state(const rhi::GpuBuffer& buffer, std::size_t i, float fallback) {
    if (buffer.mapped() == nullptr) return fallback;
    // Device writes reach the host only after an invalidate on non-coherent
    // memory (a no-op on coherent memory).
    if (!buffer.invalidate_mapped()) return fallback;
    float v = 0.0F;
    std::memcpy(&v, static_cast<const char*>(buffer.mapped()) + i * sizeof(float), sizeof(v));
    return v;
}
} // namespace

float ExposureState::adapted_luminance() const { return read_state(buffer_, 0, 0.0F); }

float ExposureState::exposure() const {
    const float e = read_state(buffer_, 1, 1.0F);
    return std::isfinite(e) && e > 0.0F ? e : 1.0F;
}

float ExposureState::key() const { return read_state(buffer_, 3, 0.18F); }

ExposureMetering ExposureMetering::from(const ExposureSettings& settings, float pre_exposure) {
    ExposureMetering m;
    m.adaptation_tau = settings.adaptation_s;
    m.key = settings.key;
    m.adaptive_key = settings.adaptive_key;
    m.min_ev100 = settings.min_ev100;
    m.max_ev100 = settings.max_ev100;
    m.center_weight = settings.center_weight;
    m.pre_exposure = pre_exposure;
    return m;
}

std::expected<ExposurePass, core::Error>
ExposurePass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                     std::shared_ptr<ExposureState> state) {
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

    auto hist = make_pipeline(cache, cooked_shader_dir + "/exposure_histogram.spv",
                              out.histogram_layout_, sizeof(HistogramParams));
    if (!hist) return std::unexpected(hist.error());
    out.histogram_pipeline_ = std::move(*hist);
    auto avg = make_pipeline(cache, cooked_shader_dir + "/exposure_average.spv",
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

    if (!state) {
        auto own = ExposureState::create(device, allocator);
        if (!own) return std::unexpected(own.error());
        state = std::move(*own);
    }
    out.state_ = std::move(state);
    return out;
}

std::expected<void, core::Error>
ExposurePass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent,
                           float dt, const ExposureMetering& metering) {
    auto hist_db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, histogram_layout_);
    if (!hist_db) return std::unexpected(hist_db.error());
    histogram_descriptor_ = std::move(*hist_db);
    auto avg_db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, average_layout_);
    if (!avg_db) return std::unexpected(avg_db.error());
    average_descriptor_ = std::move(*avg_db);

    const VkBuffer hist_buf = histogram_buffer_.handle();
    const VkDeviceAddress hist_addr = histogram_address_;
    const VkBuffer exp_buf = state_->buffer();
    const VkDeviceAddress exp_addr = state_->address();

    const float pre = finite_or(metering.pre_exposure, 1.0F);
    HistogramParams hp{kMinLogLum, 1.0F / kLogLumRange, extent.width, extent.height,
                       1.0F / std::max(pre, 1e-30F),
                       std::clamp(finite_or(metering.center_weight, 0.0F), 0.0F, 1.0F), 0.0F,
                       0.0F};
    const float lo_ev = std::min(metering.min_ev100, metering.max_ev100);
    const float hi_ev = std::max(metering.min_ev100, metering.max_ev100);
    const float low = std::clamp(finite_or(metering.low_percent, 0.1F), 0.0F, 1.0F);
    AverageParams ap{kMinLogLum,
                     kLogLumRange,
                     std::max(finite_or(dt, 0.0F), 0.0F),
                     std::max(finite_or(metering.adaptation_tau, k_adaptation_tau), 1e-3F),
                     low,
                     std::clamp(finite_or(metering.high_percent, 0.9F), low, 1.0F),
                     std::max(finite_or(metering.key, 0.18F), 1e-4F),
                     metering.adaptive_key ? 1.0F : 0.0F,
                     photometry::exposure_from_ev100_units(hi_ev),
                     photometry::exposure_from_ev100_units(lo_ev),
                     photometry::k_nits_per_unit,
                     photometry::krawczyk_key(photometry::k_adaptive_key_reference_nits),
                     -std::log2(std::max(pre, 1e-30F)),
                     metering.reset ? 1.0F : 0.0F,
                     0.0F,
                     0.0F};

    graph.add_pass("exposure", rhi::PassType::compute)
        .reads(hdr, rhi::ResourceUsage::sampled_compute)
        .execute([this, hdr, hist_buf, hist_addr, exp_buf, exp_addr, extent, hp,
                  ap](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            // The adapted luminance is shared with the other frame in flight:
            // its average dispatch wrote it and its tonemap read it. Order this
            // frame's read-modify-write after both (same queue, earlier
            // submission; the fences alone no longer cover it).
            buffer_barrier(cmd, exp_buf, kStateBytes,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            histogram_descriptor_.write_sampled_image(0, ctx.resolve(hdr).view, ro);
            histogram_descriptor_.write_storage_buffer(1, hist_addr, kHistBytes);
            average_descriptor_.write_storage_buffer(0, hist_addr, kHistBytes);
            average_descriptor_.write_storage_buffer(1, exp_addr, kStateBytes);

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

            // Make the adapted luminance visible to the tonemap fragment shader
            // and to host reads (adapted_luminance()) once the frame's fence
            // has signalled.
            buffer_barrier(cmd, exp_buf, kStateBytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
        });
    return {};
}

} // namespace engine::renderer
