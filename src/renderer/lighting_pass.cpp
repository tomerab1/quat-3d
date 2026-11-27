#include "engine/renderer/lighting_pass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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

// GPU push-constant block, mirrors LightParams in lighting.slang (240 bytes).
// The light fields match DirectionalLightParams; the camera/light matrices let
// the shader reconstruct world position and sample the shadow map. camera_pos.w
// is the shadows-enabled flag; ambient.w the IBL intensity; cloud_a/cloud_b are
// CloudSettings::pack_a/b; shadow = (depth bias, slope bias, PCF radius, 0).
struct LightingPushConstants {
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 light_view_proj{1.0F};
    glm::vec4 camera_pos{0.0F};
    glm::vec4 direction{0.0F, 0.0F, 1.0F, 0.0F};
    glm::vec4 color{1.0F};
    glm::vec4 ambient{0.0F};
    glm::vec4 cloud_a{0.0F};
    glm::vec4 cloud_b{0.0F};
    glm::vec4 shadow{0.0F};
};
static_assert(sizeof(LightingPushConstants) == 240, "must match lighting.slang LightParams");

// First of the four clustered-light bindings (params, lights, grid, indices).
constexpr std::uint32_t kClusterBinding = 15;

// Lighting descriptor set 0: GBuffer albedo/normal/material/depth as sampled
// images (read via Load — no sampler), the HDR output as a storage image, then
// the shadow map (sampled) + its sampler, the IBL maps, the clearcoat target,
// the atmosphere LUTs and the clustered lights at 15..18. Binding 7 (the old
// flat point-light list) is unused. Matches lighting.slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 18> lighting_bindings() {
    using rhi::DescriptorBinding;
    const auto clusters = clustered_light_bindings(kClusterBinding, kComputeStage);
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage}, // irradiance cube
        {0, 9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage}, // prefiltered cube
        {0, 10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage}, // BRDF LUT
        {0, 11, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},       // IBL sampler
        {0, 12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage}, // GBuffer clearcoat
        {0, 13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage}, // atmosphere sky-view
        {0, 14, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage}, // atmosphere transmittance
        clusters[0], clusters[1], clusters[2], clusters[3],
    }};
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

    const auto bindings = lighting_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/lighting.spv");
    if (!shader) return std::unexpected(shader.error());

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(LightingPushConstants)};

    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.shadow_sampler_ = std::move(*sampler);

    // Zero-light cluster set bound when the caller supplies no culled lights.
    auto empty = EmptyLightClusters::create(device, allocator);
    if (!empty) return std::unexpected(empty.error());
    out.empty_lights_ = std::move(*empty);

    // Black fallback IBL maps so bindings 8-11 are always valid (the shader only
    // samples them when the environment is enabled).
    auto fallback = IblMaps::create_fallback(device, allocator);
    if (!fallback) return std::unexpected(fallback.error());
    out.fallback_ibl_ = std::move(*fallback);

    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
LightingPass::add_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer,
                           VkExtent2D extent, const DirectionalLightParams& light,
                           const glm::mat4& inv_view_proj, const glm::vec3& camera_pos,
                           rhi::ResourceHandle shadow_map, const glm::mat4& light_view_proj,
                           const ClusteredLights& lights, bool enable_sky,
                           IblViewSet ibl, VkImageView atmosphere_skyview,
                           VkImageView atmosphere_transmittance, const CloudSettings& clouds,
                           const LightingTuning& tuning) {
    const rhi::ResourceHandle hdr = graph.create_transient_image(
        "hdr_color", hdr_color_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    // When no shadow map is supplied, sample the GBuffer depth as a harmless
    // placeholder at binding 5 (descriptors must be bound) and disable shadows.
    const bool has_shadow = shadow_map.valid();
    const rhi::ResourceHandle shadow = has_shadow ? shadow_map : gbuffer.depth;

    // Sky mode: 0 = off, 1 = procedural, 2 = atmosphere LUTs.
    const bool has_atmosphere =
        atmosphere_skyview != VK_NULL_HANDLE && atmosphere_transmittance != VK_NULL_HANDLE;
    LightingPushConstants push{};
    push.inv_view_proj = inv_view_proj;
    push.light_view_proj = light_view_proj;
    push.camera_pos = glm::vec4(camera_pos, has_shadow ? 1.0F : 0.0F);
    push.direction = glm::vec4(glm::vec3(light.direction),
                               enable_sky ? (has_atmosphere ? 2.0F : 1.0F) : 0.0F);
    push.color = light.color;
    push.ambient = glm::vec4(glm::vec3(light.ambient), std::max(tuning.ibl_intensity, 0.0F));
    push.cloud_a = clouds.pack_a();
    push.cloud_b = clouds.pack_b();
    push.shadow = glm::vec4(std::max(tuning.shadow_depth_bias, 0.0F),
                            std::max(tuning.shadow_slope_bias, 0.0F),
                            static_cast<float>(std::clamp(tuning.shadow_pcf_radius, 0, 3)), 0.0F);

    auto pass = graph.add_pass("lighting", rhi::PassType::compute);
    pass.reads(gbuffer.albedo, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.normal, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.material, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.clearcoat, rhi::ResourceUsage::sampled_compute)
        .reads(gbuffer.depth, rhi::ResourceUsage::sampled_compute)
        .writes(hdr, rhi::ResourceUsage::storage_write);
    if (has_shadow) {
        pass.reads(shadow_map, rhi::ResourceUsage::sampled_compute);
    }
    // The cull pass's grid/index writes reach this pass through the graph's
    // compute -> compute buffer barriers.
    read_clustered_lights(pass, lights, rhi::ResourceUsage::buffer_read_compute);
    const ClusteredLightBuffers cluster_buffers =
        lights.valid() ? lights.buffers : empty_lights_.buffers();
    // Non-owning view set, captured by value — no pointer lifetime to manage.
    const IblViewSet maps = ibl.valid() ? ibl : fallback_ibl_.views();
    const VkImageView skyview_view = has_atmosphere ? atmosphere_skyview : VK_NULL_HANDLE;
    const VkImageView transmittance_view =
        has_atmosphere ? atmosphere_transmittance : VK_NULL_HANDLE;
    pass.execute([this, gbuffer, hdr, shadow, extent, push, maps, skyview_view,
                  transmittance_view, cluster_buffers](rhi::PassContext& ctx) {
        const VkCommandBuffer cmd = ctx.cmd();
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frame_descriptor_.write_sampled_image(0, ctx.resolve(gbuffer.albedo).view, ro);
        frame_descriptor_.write_sampled_image(1, ctx.resolve(gbuffer.normal).view, ro);
        frame_descriptor_.write_sampled_image(2, ctx.resolve(gbuffer.material).view, ro);
        frame_descriptor_.write_sampled_image(3, ctx.resolve(gbuffer.depth).view, ro);
        frame_descriptor_.write_storage_image(4, ctx.resolve(hdr).view, VK_IMAGE_LAYOUT_GENERAL);
        frame_descriptor_.write_sampled_image(5, ctx.resolve(shadow).view, ro);
        frame_descriptor_.write_sampler(6, shadow_sampler_.handle());
        frame_descriptor_.write_sampled_image(8, maps.irradiance, ro);
        frame_descriptor_.write_sampled_image(9, maps.prefiltered, ro);
        frame_descriptor_.write_sampled_image(10, maps.brdf_lut, ro);
        frame_descriptor_.write_sampler(11, maps.sampler);
        frame_descriptor_.write_sampled_image(12, ctx.resolve(gbuffer.clearcoat).view, ro);
        // Atmosphere LUTs; the (black) fallback BRDF LUT stands in when absent
        // (the shader never samples them below sky mode 2).
        frame_descriptor_.write_sampled_image(
            13, skyview_view != VK_NULL_HANDLE ? skyview_view : maps.brdf_lut, ro);
        frame_descriptor_.write_sampled_image(
            14, transmittance_view != VK_NULL_HANDLE ? transmittance_view : maps.brdf_lut, ro);
        write_clustered_light_descriptors(frame_descriptor_, kClusterBinding, cluster_buffers);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
        frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);

        const std::uint32_t gx = (extent.width + 7) / 8;
        const std::uint32_t gy = (extent.height + 7) / 8;
        vkCmdDispatch(cmd, gx, gy, 1);
    });

    return hdr;
}

} // namespace engine::renderer
