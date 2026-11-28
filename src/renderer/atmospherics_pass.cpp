#include "engine/renderer/atmospherics_pass.hpp"

#include <cstring>
#include <utility>

#include "compute_kernel.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "tracked_image.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

enum Slot : std::uint8_t {
    a_comp, a_hdr, a_depth, a_cloud, a_cloud_depth, a_ap, a_fog, a_irradiance, a_linear, a_count
};
constexpr std::array<rhi::BindingRequest, a_count> k_bindings{{
    {"g_comp", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_hdr", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_cloud", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_cloud_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_ap", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_fog", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_irradiance", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_linear", VK_DESCRIPTOR_TYPE_SAMPLER},
}};

// Mirrors Atmospherics in atmospherics_composite.slang (std140, 192 bytes).
struct AtmosphericsGpu {
    glm::mat4 inv_view_proj{1.0F};
    glm::vec4 camera{0.0F};
    glm::vec4 flags{0.0F};
    glm::vec4 scale{0.0F};
    glm::vec4 height_fog{0.0F};
    glm::vec4 fog_albedo{0.0F};
    glm::vec4 sun{0.0F};
    glm::vec4 sun_radiance{0.0F};
    glm::vec4 ambient{0.0F};
};
static_assert(sizeof(AtmosphericsGpu) == 192, "must match atmospherics_composite.slang");

[[nodiscard]] float flag(bool b) { return b ? 1.0F : 0.0F; }

} // namespace

struct AtmosphericsPass::Fallbacks {
    detail::TrackedImage cloud;
    detail::TrackedImage cloud_depth;
    detail::TrackedImage volume; // stands in for the AP and fog volumes
    IblMaps              ibl;
};

AtmosphericsPass::AtmosphericsPass() = default;
AtmosphericsPass::~AtmosphericsPass() = default;
AtmosphericsPass::AtmosphericsPass(AtmosphericsPass&&) noexcept = default;
AtmosphericsPass& AtmosphericsPass::operator=(AtmosphericsPass&&) noexcept = default;

std::expected<AtmosphericsPass, core::Error>
AtmosphericsPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    AtmosphericsPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("atmospherics: descriptor buffers unavailable");
    auto kernel = detail::build_compute_kernel(device, out.db_fns_, cache, cooked_shader_dir,
                                               "atmospherics_composite", k_bindings, 0,
                                               "atmospherics");
    if (!kernel) return std::unexpected(kernel.error());
    out.layout_ = std::move(kernel->layout);
    out.pipeline_ = std::move(kernel->pipeline);
    out.slots_ = std::move(kernel->slots);
    auto constants = allocator.create_buffer(
        sizeof(AtmosphericsGpu),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!constants) return std::unexpected(constants.error());
    out.constants_ = std::move(*constants);
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    auto fb = std::make_unique<Fallbacks>();
    const VkDevice vk = device.handle();
    auto cloud = detail::TrackedImage::create(vk, allocator, cloud_color_format, {1, 1, 1});
    if (!cloud) return std::unexpected(cloud.error());
    fb->cloud = std::move(*cloud);
    auto cloud_depth = detail::TrackedImage::create(vk, allocator, cloud_depth_format, {1, 1, 1});
    if (!cloud_depth) return std::unexpected(cloud_depth.error());
    fb->cloud_depth = std::move(*cloud_depth);
    auto volume = detail::TrackedImage::create(vk, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                                               {1, 1, 2});
    if (!volume) return std::unexpected(volume.error());
    fb->volume = std::move(*volume);
    auto ibl = IblMaps::create_fallback(device, allocator);
    if (!ibl) return std::unexpected(ibl.error());
    fb->ibl = std::move(*ibl);
    out.fallbacks_ = std::move(fb);
    return out;
}

std::expected<void, core::Error> AtmosphericsPass::add_to_graph(rhi::RenderGraph& graph,
                                                                const AtmosphericsInputs& in) {
    if (!valid()) return fail("atmospherics: not created");
    const bool has_ap = in.aerial_perspective.valid();
    const bool has_clouds = in.clouds.valid();
    const bool has_fog = in.fog.valid();
    const bool height_fog = in.height_fog && in.media.extinction > 0.0F;
    if (!has_ap && !has_clouds && !has_fog && !height_fog) return {};
    if (!in.hdr.valid() || !in.depth.valid()) return fail("atmospherics: hdr and depth required");

    AtmosphericsGpu c;
    c.inv_view_proj = in.inv_view_proj;
    c.camera = glm::vec4(in.camera, 0.0F);
    c.flags = {flag(has_ap), flag(has_clouds), flag(has_fog), flag(height_fog)};
    c.scale = {in.sky_scale * in.pre_exposure, in.pre_exposure, in.media.max_distance_m,
               in.irradiance != VK_NULL_HANDLE ? in.media.ibl_intensity : 0.0F};
    c.height_fog = {in.media.extinction, in.media.height_falloff, in.media.base_height_m,
                    in.media.anisotropy};
    c.fog_albedo = glm::vec4(in.media.albedo, 0.0F);
    c.sun = glm::vec4(in.media.sun_to, 0.0F);
    c.sun_radiance = glm::vec4(in.media.sun_radiance, 0.0F);
    c.ambient = glm::vec4(in.media.ambient, 0.0F);
    std::memcpy(constants_.mapped(), &c, sizeof(c));
    if (auto f = constants_.flush(0, sizeof(c)); !f) return std::unexpected(f.error());

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);
    Fallbacks& fb = *fallbacks_;
    const rhi::ResourceHandle cloud =
        has_clouds ? in.clouds.color : fb.cloud.import(graph, "atmos_cloud_fallback", true);
    const rhi::ResourceHandle cloud_depth =
        has_clouds ? in.clouds.depth : fb.cloud_depth.import(graph, "atmos_cloud_depth_fallback", true);
    const rhi::ResourceHandle fallback_volume =
        (has_ap && has_fog) ? rhi::ResourceHandle{} : fb.volume.import(graph, "atmos_volume_fallback", true);
    const rhi::ResourceHandle ap = has_ap ? in.aerial_perspective : fallback_volume;
    const rhi::ResourceHandle fog = has_fog ? in.fog : fallback_volume;
    const VkImageView irradiance =
        in.irradiance != VK_NULL_HANDLE ? in.irradiance : fb.ibl.views().irradiance;

    rhi::DescriptorBuffer& d = frame_descriptor_;
    d.write_uniform_buffer(slots_[a_comp],
                           rhi::buffer_device_address(device_->handle(), constants_),
                           sizeof(AtmosphericsGpu));
    d.write_sampled_image(slots_[a_irradiance], irradiance,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    d.write_sampler(slots_[a_linear], sampler_.handle());

    auto pass = graph.add_pass("atmospherics", rhi::PassType::compute);
    pass.reads(in.depth, rhi::ResourceUsage::sampled_compute)
        .reads(cloud, rhi::ResourceUsage::sampled_compute)
        .reads(cloud_depth, rhi::ResourceUsage::sampled_compute)
        .reads(ap, rhi::ResourceUsage::sampled_compute)
        .writes(in.hdr, rhi::ResourceUsage::storage_read_write);
    if (fog.index != ap.index) pass.reads(fog, rhi::ResourceUsage::sampled_compute);
    const rhi::ResourceHandle hdr = in.hdr;
    const rhi::ResourceHandle depth = in.depth;
    const VkExtent2D extent = in.extent;
    pass.execute([this, hdr, depth, cloud, cloud_depth, ap, fog, extent](rhi::PassContext& ctx) {
        rhi::DescriptorBuffer& desc = frame_descriptor_;
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        desc.write_storage_image(slots_[a_hdr], ctx.resolve(hdr).view, VK_IMAGE_LAYOUT_GENERAL);
        desc.write_sampled_image(slots_[a_depth], ctx.resolve(depth).view, ro);
        desc.write_sampled_image(slots_[a_cloud], ctx.resolve(cloud).view, ro);
        desc.write_sampled_image(slots_[a_cloud_depth], ctx.resolve(cloud_depth).view, ro);
        desc.write_sampled_image(slots_[a_ap], ctx.resolve(ap).view, ro);
        desc.write_sampled_image(slots_[a_fog], ctx.resolve(fog).view, ro);
        const VkCommandBuffer cmd = ctx.cmd();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
        desc.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        vkCmdDispatch(cmd, detail::groups_of(extent.width), detail::groups_of(extent.height), 1);
    });
    return {};
}

} // namespace engine::renderer
