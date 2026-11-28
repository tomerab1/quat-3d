#include "engine/renderer/fog_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "compute_kernel.hpp"
#include "engine/renderer/csm.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "tracked_image.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

enum ScatterSlot : std::uint8_t {
    s_fog, s_volumes, s_shadow_map, s_shadow_cmp, s_cascades, s_cloud_shadow, s_linear,
    s_cluster_params, s_lights, s_cluster_grid, s_cluster_indices, s_irradiance, s_history,
    s_out, s_transmittance, s_count
};
constexpr std::array<rhi::BindingRequest, s_count> k_scatter_bindings{{
    {"g_fog", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_volumes", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_shadow_map", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_shadow_cmp", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_cascades", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cloud_shadow", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_linear", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_cluster_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_lights", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cluster_grid", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cluster_indices", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_irradiance", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_history", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_transmittance", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
}};

enum IntegrateSlot : std::uint8_t { i_scatter, i_integrated, i_count };
constexpr std::array<rhi::BindingRequest, i_count> k_integrate_bindings{{
    {"g_scatter", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_integrated", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

// Mirrors FogFrame in lib/fog.slang (std140, 304 bytes).
struct FogFrameGpu {
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 prev_view_proj{1.0F};
    glm::vec4 camera{0.0F};
    glm::vec4 prev_camera{0.0F};
    glm::vec4 height_fog{0.0F};
    glm::vec4 albedo{0.0F};
    glm::vec4 sun{0.0F};
    glm::vec4 sun_radiance{0.0F};
    glm::vec4 ambient{0.0F};
    glm::vec4 jitter{0.0F};
    glm::vec4 cloud_shadow_u{0.0F};
    glm::vec4 cloud_shadow_v{0.0F};
    glm::vec4 atmosphere{0.0F};
};
static_assert(sizeof(FogFrameGpu) == 304, "must match FogFrame in lib/fog.slang");

struct IntegratePush {
    glm::vec4 params{0.0F}; // far m, pre-exposure
};

constexpr VkFormat k_volume_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat k_shadow_fallback_format = VK_FORMAT_D16_UNORM;
constexpr VkExtent3D k_grid{fog_grid_x, fog_grid_y, fog_grid_z};

// Radical inverse in base b (Halton sequence).
[[nodiscard]] float halton(std::uint64_t index, std::uint32_t base) {
    float f = 1.0F;
    float r = 0.0F;
    std::uint64_t i = index + 1;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
mapped(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

} // namespace

float fog_slice_depth(float w, float far_m) {
    return fog_depth_offset_m * (std::pow(far_m / fog_depth_offset_m + 1.0F, w) - 1.0F);
}

float fog_slice_w(float depth_m, float far_m) {
    return std::log2(std::max(depth_m, 0.0F) / fog_depth_offset_m + 1.0F) /
           std::log2(far_m / fog_depth_offset_m + 1.0F);
}

FogVolumeGpu make_fog_volume(const glm::mat4& world, bool ellipsoid, float extinction,
                             const glm::vec3& albedo, float edge_fade, float height_falloff) {
    const glm::mat4 inv = glm::inverse(world);
    FogVolumeGpu v;
    // Rows of the inverse (glm is column-major: row r = (m[0][r], m[1][r], ...)).
    v.row0 = {inv[0][0], inv[1][0], inv[2][0], inv[3][0]};
    v.row1 = {inv[0][1], inv[1][1], inv[2][1], inv[3][1]};
    v.row2 = {inv[0][2], inv[1][2], inv[2][2], inv[3][2]};
    v.albedo_density = {glm::clamp(albedo, glm::vec3(0.0F), glm::vec3(1.0F)),
                        std::max(extinction, 0.0F)};
    v.params = {ellipsoid ? 1.0F : 0.0F, std::clamp(edge_fade, 0.0F, 1.0F),
                std::max(height_falloff, 0.0F), 0.0F};
    return v;
}

// ---------------------------------------------------------------------------
// FogHistory
// ---------------------------------------------------------------------------

struct FogHistory::Slot {
    detail::TrackedImage scatter;
};

FogHistory::FogHistory(const rhi::Device& device, rhi::GpuAllocator& allocator)
    : device_(&device), allocator_(&allocator) {}

FogHistory::~FogHistory() = default;

std::expected<void, core::Error> FogHistory::ensure() {
    if (slots_.size() == 2) return {};
    for (int i = 0; i < 2; ++i) {
        auto img = detail::TrackedImage::create(device_->handle(), *allocator_, k_volume_format,
                                                k_grid);
        if (!img) return std::unexpected(img.error());
        auto slot = std::make_unique<Slot>();
        slot->scatter = std::move(*img);
        slots_.push_back(std::move(slot));
    }
    valid_ = false;
    return {};
}

// ---------------------------------------------------------------------------
// FogPass
// ---------------------------------------------------------------------------

struct FogPass::Fallbacks {
    rhi::GpuImage        shadow;
    rhi::ImageView       shadow_view;
    rhi::GpuImage        cloud_shadow;
    rhi::ImageView       cloud_shadow_view;
    rhi::GpuBuffer       cascades;
    EmptyLightClusters   lights;
    IblMaps              ibl;
    detail::TrackedImage integrated;
};

FogPass::FogPass() = default;
FogPass::~FogPass() = default;
FogPass::FogPass(FogPass&&) noexcept = default;
FogPass& FogPass::operator=(FogPass&&) noexcept = default;

std::expected<FogPass, core::Error>
FogPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                std::shared_ptr<FogHistory> history) {
    if (!history) return fail("fog: no history");
    FogPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.history_ = std::move(history);
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("fog: descriptor buffers unavailable");
    auto scatter = detail::build_compute_kernel(device, out.db_fns_, cache, cooked_shader_dir,
                                                "fog_scatter", k_scatter_bindings, 0, "fog");
    if (!scatter) return std::unexpected(scatter.error());
    out.scatter_layout_ = std::move(scatter->layout);
    out.scatter_pipeline_ = std::move(scatter->pipeline);
    out.scatter_slots_ = std::move(scatter->slots);
    auto integrate = detail::build_compute_kernel(device, out.db_fns_, cache, cooked_shader_dir,
                                                  "fog_integrate", k_integrate_bindings,
                                                  sizeof(IntegratePush), "fog");
    if (!integrate) return std::unexpected(integrate.error());
    out.integrate_layout_ = std::move(integrate->layout);
    out.integrate_pipeline_ = std::move(integrate->pipeline);
    out.integrate_slots_ = std::move(integrate->slots);

    auto constants = mapped(allocator, sizeof(FogFrameGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!constants) return std::unexpected(constants.error());
    out.constants_ = std::move(*constants);
    auto volumes = mapped(allocator, sizeof(FogVolumeGpu) * fog_max_volumes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!volumes) return std::unexpected(volumes.error());
    out.volumes_ = std::move(*volumes);

    const VkDevice vk = device.handle();
    auto linear = rhi::create_sampler(vk, rhi::SamplerAddress::clamp_to_edge);
    if (!linear) return std::unexpected(linear.error());
    out.linear_sampler_ = std::move(*linear);
    rhi::SamplerDesc cmp;
    cmp.compare = true;
    cmp.compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    cmp.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto shadow_sampler = rhi::create_sampler(vk, cmp);
    if (!shadow_sampler) return std::unexpected(shadow_sampler.error());
    out.shadow_sampler_ = std::move(*shadow_sampler);

    auto fb = std::make_unique<Fallbacks>();
    auto shadow = allocator.create_image(k_shadow_fallback_format, {1, 1},
                                         VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!shadow) return std::unexpected(shadow.error());
    fb->shadow = std::move(*shadow);
    auto shadow_view = rhi::create_image_view(vk, fb->shadow.handle(), k_shadow_fallback_format,
                                              VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                                              VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, 1);
    if (!shadow_view) return std::unexpected(shadow_view.error());
    fb->shadow_view = std::move(*shadow_view);
    auto cloud = allocator.create_image(cloud_shadow_format, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!cloud) return std::unexpected(cloud.error());
    fb->cloud_shadow = std::move(*cloud);
    auto cloud_view = rhi::create_image_view(vk, fb->cloud_shadow.handle(), cloud_shadow_format);
    if (!cloud_view) return std::unexpected(cloud_view.error());
    fb->cloud_shadow_view = std::move(*cloud_view);
    auto cascades = mapped(allocator, sizeof(GpuCascades), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!cascades) return std::unexpected(cascades.error());
    fb->cascades = std::move(*cascades);
    std::memset(fb->cascades.mapped(), 0, sizeof(GpuCascades));
    if (auto f = fb->cascades.flush(); !f) return std::unexpected(f.error());
    auto lights = EmptyLightClusters::create(device, allocator);
    if (!lights) return std::unexpected(lights.error());
    fb->lights = std::move(*lights);
    auto ibl = IblMaps::create_fallback(device, allocator);
    if (!ibl) return std::unexpected(ibl.error());
    fb->ibl = std::move(*ibl);
    auto integrated = detail::TrackedImage::create(vk, allocator, k_volume_format, k_grid);
    if (!integrated) return std::unexpected(integrated.error());
    fb->integrated = std::move(*integrated);
    out.fallbacks_ = std::move(fb);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error> FogPass::add_to_graph(rhi::RenderGraph& graph,
                                                                       const FogInputs& in) {
    if (!valid()) return fail("fog: not created");
    FogHistory& hist = *history_;
    if (auto r = hist.ensure(); !r) return std::unexpected(r.error());
    if (in.reset) hist.valid_ = false;
    Fallbacks& fb = *fallbacks_;

    const bool has_shadow = in.shadows != nullptr && in.shadows->valid();
    const bool has_cloud_shadow = in.cloud_shadow != nullptr && in.cloud_shadow->valid();
    const bool has_lights = in.lights != nullptr && in.lights->valid();
    const auto count = static_cast<std::uint32_t>(
        std::min<std::size_t>(in.volumes.size(), fog_max_volumes));
    const float far_m = std::max(in.media.max_distance_m, 16.0F);

    FogFrameGpu f;
    f.inv_view_proj = in.inv_view_proj;
    f.prev_view_proj = in.prev_view_proj;
    f.camera = {in.camera, far_m};
    f.prev_camera = {in.prev_camera, hist.valid_ ? 1.0F : 0.0F};
    f.height_fog = {std::max(in.media.extinction, 0.0F), std::max(in.media.height_falloff, 0.0F),
                    in.media.base_height_m, std::clamp(in.media.anisotropy, -0.95F, 0.95F)};
    f.albedo = {glm::clamp(in.media.albedo, glm::vec3(0.0F), glm::vec3(1.0F)),
                k_fog_temporal_blend};
    f.sun = {in.media.sun_to, has_shadow ? 1.0F : 0.0F};
    f.sun_radiance = {in.media.sun_radiance, has_cloud_shadow ? 1.0F : 0.0F};
    f.ambient = {in.media.ambient, static_cast<float>(count)};
    f.jitter = {halton(in.frame_index % 1024U, 2), halton(in.frame_index % 1024U, 3),
                halton(in.frame_index % 1024U, 5),
                in.irradiance != VK_NULL_HANDLE ? in.media.ibl_intensity : 0.0F};
    if (has_cloud_shadow) {
        f.cloud_shadow_u = in.cloud_shadow->projection.u_axis;
        f.cloud_shadow_v = in.cloud_shadow->projection.v_axis;
    }
    f.atmosphere = {in.transmittance != VK_NULL_HANDLE ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F};
    std::memcpy(constants_.mapped(), &f, sizeof(f));
    if (auto r = constants_.flush(0, sizeof(f)); !r) return std::unexpected(r.error());
    if (count > 0) {
        std::memcpy(volumes_.mapped(), in.volumes.data(), sizeof(FogVolumeGpu) * count);
        if (auto r = volumes_.flush(0, sizeof(FogVolumeGpu) * count); !r) {
            return std::unexpected(r.error());
        }
    }

    auto sdb = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, scatter_layout_);
    if (!sdb) return std::unexpected(sdb.error());
    scatter_descriptor_ = std::move(*sdb);
    auto idb = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, integrate_layout_);
    if (!idb) return std::unexpected(idb.error());
    integrate_descriptor_ = std::move(*idb);

    const VkDevice vk = device_->handle();
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const rhi::ResourceHandle shadow_map =
        has_shadow ? in.shadows->shadow_map
                   : graph.import_image("fog_csm_fallback", fb.shadow.handle(),
                                        fb.shadow_view.handle(), k_shadow_fallback_format, {1, 1},
                                        VK_IMAGE_LAYOUT_UNDEFINED, ro);
    const rhi::ResourceHandle cloud_map =
        has_cloud_shadow ? in.cloud_shadow->texture
                         : graph.import_image("fog_cloud_shadow_fallback", fb.cloud_shadow.handle(),
                                              fb.cloud_shadow_view.handle(), cloud_shadow_format,
                                              {1, 1}, VK_IMAGE_LAYOUT_UNDEFINED, ro);
    const VkDeviceAddress cascades = has_shadow ? in.shadows->cascades_address
                                                : rhi::buffer_device_address(vk, fb.cascades);
    const ClusteredLightBuffers cl = has_lights ? in.lights->buffers : fb.lights.buffers();
    const VkImageView irradiance =
        in.irradiance != VK_NULL_HANDLE ? in.irradiance : fb.ibl.views().irradiance;
    {
        rhi::DescriptorBuffer& d = scatter_descriptor_;
        const auto& s = scatter_slots_;
        d.write_uniform_buffer(s[s_fog], rhi::buffer_device_address(vk, constants_),
                               sizeof(FogFrameGpu));
        d.write_storage_buffer(s[s_volumes], rhi::buffer_device_address(vk, volumes_),
                               sizeof(FogVolumeGpu) * fog_max_volumes);
        d.write_sampler(s[s_shadow_cmp], shadow_sampler_.handle());
        d.write_storage_buffer(s[s_cascades], cascades, sizeof(GpuCascades));
        d.write_sampler(s[s_linear], linear_sampler_.handle());
        d.write_uniform_buffer(s[s_cluster_params], cl.params, cl.params_size);
        d.write_storage_buffer(s[s_lights], cl.lights, cl.lights_size);
        d.write_storage_buffer(s[s_cluster_grid], cl.grid, cl.grid_size);
        d.write_storage_buffer(s[s_cluster_indices], cl.indices, cl.indices_size);
        d.write_sampled_image(s[s_irradiance], irradiance, ro);
        // Without the LUT atmosphere the (black, never read) BRDF LUT stands in.
        d.write_sampled_image(s[s_transmittance],
                              in.transmittance != VK_NULL_HANDLE ? in.transmittance
                                                                 : fb.ibl.views().brdf_lut,
                              ro);
    }

    FogHistory::Slot& out_slot = *hist.slots_[hist.frame_ % 2];
    FogHistory::Slot& prev_slot = *hist.slots_[(hist.frame_ + 1) % 2];
    const rhi::ResourceHandle out_h = out_slot.scatter.import(graph, "fog_scatter_out", false);
    const rhi::ResourceHandle prev_h = prev_slot.scatter.import(graph, "fog_scatter_history", false);
    const rhi::ResourceHandle integrated = fb.integrated.import(graph, "fog_integrated", true);

    auto pass = graph.add_pass("fog_scatter", rhi::PassType::compute);
    pass.reads(shadow_map, rhi::ResourceUsage::sampled_compute)
        .reads(cloud_map, rhi::ResourceUsage::sampled_compute)
        .reads(prev_h, rhi::ResourceUsage::sampled_compute)
        .writes(out_h, rhi::ResourceUsage::storage_write);
    if (has_shadow) pass.reads(in.shadows->cascades, rhi::ResourceUsage::buffer_read_compute);
    if (has_lights) read_clustered_lights(pass, *in.lights, rhi::ResourceUsage::buffer_read_compute);
    pass.execute([this, shadow_map, cloud_map, prev_h, out_h](rhi::PassContext& ctx) {
        rhi::DescriptorBuffer& d = scatter_descriptor_;
        const auto& s = scatter_slots_;
        constexpr VkImageLayout ro_l = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        d.write_sampled_image(s[s_shadow_map], ctx.resolve(shadow_map).view, ro_l);
        d.write_sampled_image(s[s_cloud_shadow], ctx.resolve(cloud_map).view, ro_l);
        d.write_sampled_image(s[s_history], ctx.resolve(prev_h).view, ro_l);
        d.write_storage_image(s[s_out], ctx.resolve(out_h).view, VK_IMAGE_LAYOUT_GENERAL);
        const VkCommandBuffer cmd = ctx.cmd();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatter_pipeline_.handle());
        d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatter_pipeline_.layout(), 0);
        vkCmdDispatch(cmd, detail::groups_of(fog_grid_x), detail::groups_of(fog_grid_y),
                      fog_grid_z);
    });
    IntegratePush push;
    push.params = {far_m, std::max(in.pre_exposure, 0.0F), 0.0F, 0.0F};
    (void)graph.add_pass("fog_integrate", rhi::PassType::compute)
        .reads(out_h, rhi::ResourceUsage::sampled_compute)
        .writes(integrated, rhi::ResourceUsage::storage_write)
        .execute([this, out_h, integrated, push](rhi::PassContext& ctx) {
            rhi::DescriptorBuffer& d = integrate_descriptor_;
            constexpr VkImageLayout ro_l = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            d.write_sampled_image(integrate_slots_[i_scatter], ctx.resolve(out_h).view, ro_l);
            d.write_storage_image(integrate_slots_[i_integrated], ctx.resolve(integrated).view,
                                  VK_IMAGE_LAYOUT_GENERAL);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_.handle());
            d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrate_pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, integrate_pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(cmd, detail::groups_of(fog_grid_x), detail::groups_of(fog_grid_y), 1);
        });
    hist.valid_ = true;
    ++hist.frame_;
    return integrated;
}

} // namespace engine::renderer
