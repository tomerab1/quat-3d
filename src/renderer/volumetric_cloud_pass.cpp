#include "engine/renderer/volumetric_cloud_pass.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "compute_kernel.hpp"
#include "engine/core/profile.hpp"
#include "engine/renderer/cloud_noise_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "tracked_image.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

enum MarchSlot : std::uint8_t {
    m_layer, m_march, m_base, m_detail, m_curl, m_weather, m_noise_sampler, m_transmittance,
    m_skyview, m_ap, m_lut_sampler, m_depth, m_cloud, m_cloud_depth, m_count
};
constexpr std::array<rhi::BindingRequest, m_count> k_march_bindings{{
    {"g_layer", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_march", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_base", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_detail", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_curl", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_weather", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_noise_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_transmittance", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_skyview", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_ap", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_lut_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_cloud", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_cloud_depth", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

enum TemporalSlot : std::uint8_t {
    t_frame, t_current, t_current_depth, t_history, t_history_depth, t_sampler, t_out,
    t_out_depth, t_count
};
constexpr std::array<rhi::BindingRequest, t_count> k_temporal_bindings{{
    {"g_temporal", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_current", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_current_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_history", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_history_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_out_depth", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

// Mirrors CloudMarch in cloud_march.slang (std140, 112 bytes).
struct CloudMarchGpu {
    glm::mat4 inv_view_proj{1.0F};
    glm::vec4 camera{0.0F};
    glm::vec4 extent{0.0F};
    glm::vec4 frame{0.0F};
};
static_assert(sizeof(CloudMarchGpu) == 112, "must match cloud_march.slang CloudMarch");

// Mirrors CloudTemporal in cloud_temporal.slang (std140, 176 bytes).
struct CloudTemporalGpu {
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 prev_view_proj{1.0F};
    glm::vec4 camera{0.0F};
    glm::vec4 drift{0.0F};
    glm::vec4 extent{0.0F};
};
static_assert(sizeof(CloudTemporalGpu) == 176, "must match cloud_temporal.slang CloudTemporal");

// Offsets of the three uniform blocks in constants_ (256 = the largest
// minUniformBufferOffsetAlignment Vulkan allows).
constexpr VkDeviceSize k_layer_offset = 0;
constexpr VkDeviceSize k_march_offset = 256;
constexpr VkDeviceSize k_temporal_offset = 512;
constexpr VkDeviceSize k_constants_size = 768;

} // namespace

// ---------------------------------------------------------------------------
// CloudHistory
// ---------------------------------------------------------------------------

struct CloudHistory::Slot {
    detail::TrackedImage color;
    detail::TrackedImage depth;
};

CloudHistory::CloudHistory(const rhi::Device& device, rhi::GpuAllocator& allocator,
                           std::uint32_t frames_in_flight)
    : device_(&device), allocator_(&allocator),
      frames_in_flight_(frames_in_flight == 0 ? 1 : frames_in_flight) {}

CloudHistory::~CloudHistory() = default;

std::expected<void, core::Error> CloudHistory::ensure(VkExtent2D extent) {
    std::erase_if(retired_, [this](const auto& r) { return frame_ >= r.first + frames_in_flight_; });
    if (extent_.width == extent.width && extent_.height == extent.height && slots_.size() == 2) {
        return {};
    }
    QUAT_ZONE_N("cloud_history_resize");
    // The other frame in flight may still use the old pair: retire it.
    if (!slots_.empty()) retired_.emplace_back(frame_, std::move(slots_));
    slots_.clear();
    for (int i = 0; i < 2; ++i) {
        auto slot = std::make_unique<Slot>();
        auto c = detail::TrackedImage::create(device_->handle(), *allocator_, cloud_color_format,
                                              {extent.width, extent.height, 1});
        if (!c) return std::unexpected(c.error());
        auto d = detail::TrackedImage::create(device_->handle(), *allocator_, cloud_depth_format,
                                              {extent.width, extent.height, 1});
        if (!d) return std::unexpected(d.error());
        slot->color = std::move(*c);
        slot->depth = std::move(*d);
        slots_.push_back(std::move(slot));
    }
    extent_ = extent;
    valid_ = false;
    return {};
}

// ---------------------------------------------------------------------------
// VolumetricCloudPass
// ---------------------------------------------------------------------------

std::expected<VolumetricCloudPass, core::Error>
VolumetricCloudPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                            rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                            std::shared_ptr<CloudHistory> history) {
    if (!history) return fail("volumetric clouds: no history");
    VolumetricCloudPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.history_ = std::move(history);
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("volumetric clouds: descriptor buffers unavailable");
    auto march = detail::build_compute_kernel(device, out.db_fns_, cache, cooked_shader_dir,
                                              "cloud_march", k_march_bindings, 0,
                                              "volumetric clouds");
    if (!march) return std::unexpected(march.error());
    out.march_layout_ = std::move(march->layout);
    out.march_pipeline_ = std::move(march->pipeline);
    out.march_slots_ = std::move(march->slots);
    auto temporal = detail::build_compute_kernel(device, out.db_fns_, cache, cooked_shader_dir,
                                                 "cloud_temporal", k_temporal_bindings, 0,
                                                 "volumetric clouds");
    if (!temporal) return std::unexpected(temporal.error());
    out.temporal_layout_ = std::move(temporal->layout);
    out.temporal_pipeline_ = std::move(temporal->pipeline);
    out.temporal_slots_ = std::move(temporal->slots);

    auto constants = allocator.create_buffer(
        k_constants_size,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!constants) return std::unexpected(constants.error());
    out.constants_ = std::move(*constants);
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.clamp_sampler_ = std::move(*sampler);
    return out;
}

std::expected<CloudImage, core::Error>
VolumetricCloudPass::add_to_graph(rhi::RenderGraph& graph, const VolumetricCloudInputs& in,
                                  const CloudNoisePass& noise) {
    if (!valid()) return fail("volumetric clouds: not created");
    if (!noise.valid()) return fail("volumetric clouds: no cloud noise");
    if (in.transmittance == VK_NULL_HANDLE || in.skyview == VK_NULL_HANDLE ||
        !in.aerial_perspective.valid() || !in.depth.valid()) {
        return fail("volumetric clouds: atmosphere LUTs, aerial perspective and depth required");
    }
    if (in.render_extent.width == 0 || in.render_extent.height == 0) {
        return fail("volumetric clouds: empty render extent");
    }
    const VkExtent2D extent = cloud_extent(in.render_extent);
    CloudHistory& hist = *history_;
    if (auto r = hist.ensure(extent); !r) return std::unexpected(r.error());
    if (in.reset) hist.valid_ = false;

    // Constants.
    CloudMarchGpu march;
    march.inv_view_proj = in.inv_view_proj;
    march.camera = glm::vec4(in.camera, 0.0F);
    march.extent = {static_cast<float>(extent.width), static_cast<float>(extent.height),
                    static_cast<float>(in.render_extent.width),
                    static_cast<float>(in.render_extent.height)};
    march.frame = {static_cast<float>(in.frame_index % 65536U),
                   static_cast<float>(std::clamp(in.march_steps, 8, 96)), 0.0F, 0.0F};
    CloudTemporalGpu temporal;
    temporal.inv_view_proj = in.inv_view_proj;
    temporal.prev_view_proj = in.prev_view_proj;
    temporal.camera = glm::vec4(in.camera, hist.valid_ ? 1.0F : 0.0F);
    temporal.drift = {in.drift_m.x, in.drift_m.y, k_cloud_temporal_blend, 0.0F};
    temporal.extent = {static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F,
                       0.0F};
    auto* bytes = static_cast<std::byte*>(constants_.mapped());
    std::memcpy(bytes + k_layer_offset, &in.layer, sizeof(in.layer));
    std::memcpy(bytes + k_march_offset, &march, sizeof(march));
    std::memcpy(bytes + k_temporal_offset, &temporal, sizeof(temporal));
    if (auto f = constants_.flush(0, k_constants_size); !f) return std::unexpected(f.error());
    const VkDeviceAddress base = rhi::buffer_device_address(device_->handle(), constants_);

    auto mdb = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, march_layout_);
    if (!mdb) return std::unexpected(mdb.error());
    march_descriptor_ = std::move(*mdb);
    auto tdb = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, temporal_layout_);
    if (!tdb) return std::unexpected(tdb.error());
    temporal_descriptor_ = std::move(*tdb);
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        rhi::DescriptorBuffer& d = march_descriptor_;
        const auto& s = march_slots_;
        d.write_uniform_buffer(s[m_layer], base + k_layer_offset, sizeof(CloudLayerGpu));
        d.write_uniform_buffer(s[m_march], base + k_march_offset, sizeof(CloudMarchGpu));
        d.write_sampled_image(s[m_base], noise.base_view(), ro);
        d.write_sampled_image(s[m_detail], noise.detail_view(), ro);
        d.write_sampled_image(s[m_curl], noise.curl_view(), ro);
        d.write_sampled_image(s[m_weather], noise.weather_view(), ro);
        d.write_sampler(s[m_noise_sampler], noise.sampler());
        d.write_sampled_image(s[m_transmittance], in.transmittance, ro);
        d.write_sampled_image(s[m_skyview], in.skyview, ro);
        d.write_sampler(s[m_lut_sampler], clamp_sampler_.handle());
    }
    {
        rhi::DescriptorBuffer& d = temporal_descriptor_;
        d.write_uniform_buffer(temporal_slots_[t_frame], base + k_temporal_offset,
                               sizeof(CloudTemporalGpu));
        d.write_sampler(temporal_slots_[t_sampler], clamp_sampler_.handle());
    }

    // Images: this frame's march (transients), the history pair.
    const rhi::ResourceHandle cur = graph.create_transient_image(
        "cloud_march", cloud_color_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const rhi::ResourceHandle cur_depth = graph.create_transient_image(
        "cloud_march_depth", cloud_depth_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    CloudHistory::Slot& out_slot = *hist.slots_[hist.frame_ % 2];
    CloudHistory::Slot& prev_slot = *hist.slots_[(hist.frame_ + 1) % 2];
    const rhi::ResourceHandle out_color = out_slot.color.import(graph, "cloud_history_out", false);
    const rhi::ResourceHandle out_depth = out_slot.depth.import(graph, "cloud_history_out_depth", false);
    const rhi::ResourceHandle prev_color = prev_slot.color.import(graph, "cloud_history", false);
    const rhi::ResourceHandle prev_depth = prev_slot.depth.import(graph, "cloud_history_depth", false);

    const rhi::ResourceHandle depth = in.depth;
    const rhi::ResourceHandle ap = in.aerial_perspective;
    const std::array<std::uint32_t, 2> march_groups{detail::groups_of(extent.width),
                                                     detail::groups_of(extent.height)};
    (void)graph.add_pass("cloud_march", rhi::PassType::compute)
        .reads(depth, rhi::ResourceUsage::sampled_compute)
        .reads(ap, rhi::ResourceUsage::sampled_compute)
        .writes(cur, rhi::ResourceUsage::storage_write)
        .writes(cur_depth, rhi::ResourceUsage::storage_write)
        .execute([this, depth, ap, cur, cur_depth, march_groups](rhi::PassContext& ctx) {
            rhi::DescriptorBuffer& d = march_descriptor_;
            const auto& s = march_slots_;
            constexpr VkImageLayout ro_l = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            d.write_sampled_image(s[m_depth], ctx.resolve(depth).view, ro_l);
            d.write_sampled_image(s[m_ap], ctx.resolve(ap).view, ro_l);
            d.write_storage_image(s[m_cloud], ctx.resolve(cur).view, VK_IMAGE_LAYOUT_GENERAL);
            d.write_storage_image(s[m_cloud_depth], ctx.resolve(cur_depth).view,
                                  VK_IMAGE_LAYOUT_GENERAL);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_.handle());
            d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_.layout(), 0);
            vkCmdDispatch(cmd, march_groups[0], march_groups[1], 1);
        });
    (void)graph.add_pass("cloud_temporal", rhi::PassType::compute)
        .reads(cur, rhi::ResourceUsage::sampled_compute)
        .reads(cur_depth, rhi::ResourceUsage::sampled_compute)
        .reads(prev_color, rhi::ResourceUsage::sampled_compute)
        .reads(prev_depth, rhi::ResourceUsage::sampled_compute)
        .writes(out_color, rhi::ResourceUsage::storage_write)
        .writes(out_depth, rhi::ResourceUsage::storage_write)
        .execute([this, cur, cur_depth, prev_color, prev_depth, out_color, out_depth,
                  march_groups](rhi::PassContext& ctx) {
            rhi::DescriptorBuffer& d = temporal_descriptor_;
            const auto& s = temporal_slots_;
            constexpr VkImageLayout ro_l = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            d.write_sampled_image(s[t_current], ctx.resolve(cur).view, ro_l);
            d.write_sampled_image(s[t_current_depth], ctx.resolve(cur_depth).view, ro_l);
            d.write_sampled_image(s[t_history], ctx.resolve(prev_color).view, ro_l);
            d.write_sampled_image(s[t_history_depth], ctx.resolve(prev_depth).view, ro_l);
            d.write_storage_image(s[t_out], ctx.resolve(out_color).view, VK_IMAGE_LAYOUT_GENERAL);
            d.write_storage_image(s[t_out_depth], ctx.resolve(out_depth).view,
                                  VK_IMAGE_LAYOUT_GENERAL);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_.handle());
            d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_.layout(), 0);
            vkCmdDispatch(cmd, march_groups[0], march_groups[1], 1);
        });
    hist.valid_ = true;
    ++hist.frame_;
    return CloudImage{out_color, out_depth};
}

} // namespace engine::renderer
