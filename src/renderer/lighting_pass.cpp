#include "engine/renderer/lighting_pass.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "engine/renderer/ibl_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

// Mirrors LightingConstants in lighting.slang (std140, 288 bytes).
struct LightingConstants {
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 view_proj{1.0F};
    glm::vec4 camera_pos{0.0F}; // w = sun shadows (CSM) on
    glm::vec4 direction{0.0F, 0.0F, 1.0F, 0.0F}; // w = sky mode
    glm::vec4 color{1.0F};
    glm::vec4 ambient{0.0F};
    glm::vec4 cloud_a{0.0F};
    glm::vec4 cloud_b{0.0F};
    glm::vec4 csm_filter{0.0F}; // PCF radius, tan(sun size), normal bias, depth bias
    glm::vec4 csm_misc{0.0F};   // debug cascades, noise offset, depth -> view (A, B)
    glm::vec4 contact{0.0F};    // intensity (0 = off), length, steps, thickness
    glm::vec4 ao{0.0F};         // on, bent normals, specular occlusion, multi-bounce
};
static_assert(sizeof(LightingConstants) == 288, "must match lighting.slang LightingConstants");

// lighting.slang's resources by name; slots_ holds their reflected bindings
// in this order.
enum Slot : std::uint8_t {
    s_frame, s_albedo, s_normal, s_material, s_depth, s_hdr, s_clearcoat, s_shadow_map,
    s_shadow_cmp, s_point, s_cascades, s_ao, s_irradiance, s_prefiltered, s_brdf_lut,
    s_ibl_sampler, s_skyview, s_transmittance, s_cluster_params, s_lights, s_cluster_grid,
    s_cluster_indices, s_count
};
constexpr std::array<rhi::BindingRequest, s_count> kResources{{
    {"g_frame", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_albedo", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_normal", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_material", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_hdr", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_clearcoat", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_shadow_map", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_shadow_cmp", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_point_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_cascades", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_ao", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_irradiance", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_prefiltered", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_brdf_lut", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_ibl_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_skyview", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_transmittance_lut", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_cluster_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_lights", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cluster_grid", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cluster_indices", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
mapped_buffer(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

[[nodiscard]] float flag(bool b) { return b ? 1.0F : 0.0F; }

// The per-frame constants for `occlusion` (the occlusion half of the block).
void fill_occlusion(LightingConstants& c, const LightingOcclusion& occlusion) {
    const CsmSettings csm = occlusion.csm.sanitized();
    const bool shadows = occlusion.shadows.valid();
    c.camera_pos.w = flag(shadows);
    const float sun_tan = std::tan(glm::radians(csm.light_size_deg));
    c.csm_filter = {csm.pcf_radius, sun_tan, csm.normal_bias, csm.depth_bias};
    // Noise offset: a different interleaved-gradient pattern each frame (the
    // 5.588238 step of Jimenez 2014), for TAA to accumulate.
    const float noise_offset =
        5.588238F * static_cast<float>(occlusion.frame_index % 64U);
    const glm::mat4& p = occlusion.projection;
    const bool perspective = p[2][3] == -1.0F && p[3][3] == 0.0F;
    c.csm_misc = {flag(shadows && csm.debug_cascades), noise_offset, p[2][2], p[3][2]};

    const ContactShadowSettings contact = occlusion.contact.sanitized();
    c.contact = {contact.enabled && perspective ? contact.intensity : 0.0F, contact.length,
                 static_cast<float>(contact.steps), contact.thickness};

    const GtaoSettings gtao = occlusion.gtao.sanitized();
    c.ao = {flag(occlusion.ao.valid()), flag(gtao.bent_normals), flag(gtao.specular_occlusion),
            flag(gtao.multi_bounce)};
}

} // namespace

std::expected<LightingPass, core::Error>
LightingPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    LightingPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("lighting pass: descriptor buffer functions unavailable");
    }

    auto shader = cache.load(cooked_shader_dir + "/lighting.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kComputeStage);
    if (!bindings) return fail("lighting pass: " + bindings.error().message);
    for (std::size_t i = 0; i < s_count; ++i) out.slots_[i] = (*bindings)[i].binding;
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    const VkDevice vk = device.handle();
    rhi::SamplerDesc cmp;
    cmp.compare = true;
    cmp.compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    cmp.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto cmp_sampler = rhi::create_sampler(vk, cmp);
    if (!cmp_sampler) return std::unexpected(cmp_sampler.error());
    out.shadow_cmp_sampler_ = std::move(*cmp_sampler);
    rhi::SamplerDesc point;
    point.filter = VK_FILTER_NEAREST;
    point.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto point_sampler = rhi::create_sampler(vk, point);
    if (!point_sampler) return std::unexpected(point_sampler.error());
    out.point_sampler_ = std::move(*point_sampler);

    auto constants = mapped_buffer(allocator, sizeof(LightingConstants),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!constants) return std::unexpected(constants.error());
    out.constants_ = std::move(*constants);

    // Zero-light cluster set bound when the caller supplies no culled lights.
    auto empty = EmptyLightClusters::create(device, allocator);
    if (!empty) return std::unexpected(empty.error());
    out.empty_lights_ = std::move(*empty);

    // Black fallback IBL maps so the IBL bindings are always valid (the shader
    // only samples them when the environment is enabled).
    auto fallback = IblMaps::create_fallback(device, allocator);
    if (!fallback) return std::unexpected(fallback.error());
    out.fallback_ibl_ = std::move(*fallback);

    // Unshadowed frames bind a 1x1 depth array + zero cascades (never read).
    auto shadow = allocator.create_image(shadow_map_format, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!shadow) return std::unexpected(shadow.error());
    out.fallback_shadow_ = std::move(*shadow);
    auto shadow_view = rhi::create_image_view(vk, out.fallback_shadow_.handle(),
                                              shadow_map_format, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                                              VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, 1);
    if (!shadow_view) return std::unexpected(shadow_view.error());
    out.fallback_shadow_view_ = std::move(*shadow_view);
    auto cascades =
        mapped_buffer(allocator, sizeof(GpuCascades), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!cascades) return std::unexpected(cascades.error());
    out.fallback_cascades_ = std::move(*cascades);
    std::memset(out.fallback_cascades_.mapped(), 0, sizeof(GpuCascades));
    if (auto f = out.fallback_cascades_.flush(); !f) return std::unexpected(f.error());
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
LightingPass::add_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer,
                           VkExtent2D extent, const DirectionalLightParams& light,
                           const glm::mat4& inv_view_proj, const glm::vec3& camera_pos,
                           const LightingOcclusion& occlusion, const ClusteredLights& lights,
                           bool enable_sky, IblViewSet ibl, VkImageView atmosphere_skyview,
                           VkImageView atmosphere_transmittance, const CloudSettings& clouds) {
    const rhi::ResourceHandle hdr = graph.create_transient_image(
        "hdr_color", hdr_color_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    // Sky mode: 0 = off, 1 = procedural, 2 = atmosphere LUTs.
    const bool has_atmosphere =
        atmosphere_skyview != VK_NULL_HANDLE && atmosphere_transmittance != VK_NULL_HANDLE;
    LightingConstants c{};
    c.inv_view_proj = inv_view_proj;
    c.view_proj = glm::inverse(inv_view_proj);
    c.camera_pos = glm::vec4(camera_pos, 0.0F);
    c.direction = glm::vec4(glm::vec3(light.direction),
                            enable_sky ? (has_atmosphere ? 2.0F : 1.0F) : 0.0F);
    c.color = light.color;
    c.ambient = glm::vec4(glm::vec3(light.ambient), 0.0F);
    c.cloud_a = clouds.pack_a();
    c.cloud_b = clouds.pack_b();
    fill_occlusion(c, occlusion);
    // Host writes land before vkQueueSubmit, which makes them visible.
    std::memcpy(constants_.mapped(), &c, sizeof(c));
    if (auto f = constants_.flush(0, sizeof(c)); !f) return std::unexpected(f.error());

    const bool has_shadow = occlusion.shadows.valid();
    const rhi::ResourceHandle shadow_map =
        has_shadow ? occlusion.shadows.shadow_map
                   : graph.import_image("csm_fallback", fallback_shadow_.handle(),
                                        fallback_shadow_view_.handle(), shadow_map_format, {1, 1},
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkDeviceAddress cascades =
        has_shadow ? occlusion.shadows.cascades_address
                   : rhi::buffer_device_address(device_->handle(), fallback_cascades_);
    const rhi::ResourceHandle ao = occlusion.ao;

    auto pass = graph.add_pass("lighting", rhi::PassType::compute);
    pass.reads(gbuffer.albedo, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.normal, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.material, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.clearcoat, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.depth, rhi::ResourceUsage::sampled_compute)
        .reads(shadow_map, rhi::ResourceUsage::sampled_compute)
        .writes(hdr, rhi::ResourceUsage::storage_write);
    if (has_shadow) {
        pass.reads(occlusion.shadows.cascades, rhi::ResourceUsage::buffer_read_compute);
    }
    if (ao.valid()) pass.reads(ao, rhi::ResourceUsage::sampled_compute);
    // The cull pass's grid/index writes reach this pass through the graph's
    // compute -> compute buffer barriers.
    read_clustered_lights(pass, lights, rhi::ResourceUsage::buffer_read_compute);
    const ClusteredLightBuffers cl = lights.valid() ? lights.buffers : empty_lights_.buffers();
    // Non-owning view set, captured by value — no pointer lifetime to manage.
    const IblViewSet maps = ibl.valid() ? ibl : fallback_ibl_.views();
    const VkImageView skyview_view = has_atmosphere ? atmosphere_skyview : VK_NULL_HANDLE;
    const VkImageView transmittance_view =
        has_atmosphere ? atmosphere_transmittance : VK_NULL_HANDLE;
    const VkDeviceAddress constants_address =
        rhi::buffer_device_address(device_->handle(), constants_);
    pass.execute([this, gbuffer, hdr, shadow_map, cascades, ao, extent, maps, skyview_view,
                  transmittance_view, cl, constants_address](rhi::PassContext& ctx) {
        const VkCommandBuffer cmd = ctx.cmd();
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rhi::DescriptorBuffer& d = frame_descriptor_;
        const auto& s = slots_;
        d.write_uniform_buffer(s[s_frame], constants_address, sizeof(LightingConstants));
        d.write_sampled_image(s[s_albedo], ctx.resolve(gbuffer.albedo).view, ro);
        d.write_sampled_image(s[s_normal], ctx.resolve(gbuffer.normal).view, ro);
        d.write_sampled_image(s[s_material], ctx.resolve(gbuffer.material).view, ro);
        d.write_sampled_image(s[s_depth], ctx.resolve(gbuffer.depth).view, ro);
        d.write_storage_image(s[s_hdr], ctx.resolve(hdr).view, VK_IMAGE_LAYOUT_GENERAL);
        d.write_sampled_image(s[s_clearcoat], ctx.resolve(gbuffer.clearcoat).view, ro);
        d.write_sampled_image(s[s_shadow_map], ctx.resolve(shadow_map).view, ro);
        d.write_sampler(s[s_shadow_cmp], shadow_cmp_sampler_.handle());
        d.write_sampler(s[s_point], point_sampler_.handle());
        d.write_storage_buffer(s[s_cascades], cascades, sizeof(GpuCascades));
        // Without GTAO the (black) fallback BRDF LUT stands in (never read).
        d.write_sampled_image(s[s_ao], ao.valid() ? ctx.resolve(ao).view : maps.brdf_lut, ro);
        d.write_sampled_image(s[s_irradiance], maps.irradiance, ro);
        d.write_sampled_image(s[s_prefiltered], maps.prefiltered, ro);
        d.write_sampled_image(s[s_brdf_lut], maps.brdf_lut, ro);
        d.write_sampler(s[s_ibl_sampler], maps.sampler);
        // Atmosphere LUTs; the (black) fallback BRDF LUT stands in when absent
        // (the shader never samples them below sky mode 2).
        d.write_sampled_image(s[s_skyview],
                              skyview_view != VK_NULL_HANDLE ? skyview_view : maps.brdf_lut, ro);
        d.write_sampled_image(
            s[s_transmittance],
            transmittance_view != VK_NULL_HANDLE ? transmittance_view : maps.brdf_lut, ro);
        d.write_uniform_buffer(s[s_cluster_params], cl.params, cl.params_size);
        d.write_storage_buffer(s[s_lights], cl.lights, cl.lights_size);
        d.write_storage_buffer(s[s_cluster_grid], cl.grid, cl.grid_size);
        d.write_storage_buffer(s[s_cluster_indices], cl.indices, cl.indices_size);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
        d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        const std::uint32_t gx = (extent.width + 7) / 8;
        const std::uint32_t gy = (extent.height + 7) / 8;
        vkCmdDispatch(cmd, gx, gy, 1);
    });

    return hdr;
}

} // namespace engine::renderer
