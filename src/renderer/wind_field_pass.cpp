#include "engine/renderer/wind_field_pass.hpp"

#include <cmath>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr std::array<rhi::BindingRequest, 1> kResources{{
    {"g_wind", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

constexpr float kExtentM = wind_field_texel_m * static_cast<float>(wind_field_resolution);

} // namespace

WindFieldGpu pack_wind_field(const weather::WindFieldParams& p, glm::vec2 corner_xz,
                             float texel_m) {
    WindFieldGpu g;
    g.flow = {p.direction, p.speed, p.gustiness};
    g.layers = {p.turbulence, p.gust_offset, p.swirl_offset, p.swirl_phase};
    // The same constants the CPU reference multiplies by (no GPU division).
    g.scales = {1.0F / weather::k_gust_along_m, 1.0F / weather::k_gust_across_m,
                1.0F / weather::k_turbulence_m, 0.0F};
    g.grid = {corner_xz, texel_m, 0.0F};
    return g;
}

glm::vec2 wind_field_corner(glm::vec2 camera_xz) {
    const glm::vec2 snapped = glm::floor(camera_xz / wind_field_texel_m) * wind_field_texel_m;
    return snapped - glm::vec2(0.5F * kExtentM);
}

std::expected<WindFieldPass, core::Error>
WindFieldPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                      rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    WindFieldPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("wind field pass: descriptor buffer functions unavailable");

    auto shader = cache.load(cooked_shader_dir + "/wind_field.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings =
        rhi::reflected_bindings((*shader)->reflection, kResources, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!bindings) return fail("wind field pass: " + bindings.error().message);
    out.out_slot_ = (*bindings)[0].binding;
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WindFieldGpu)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&push, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);
    return out;
}

std::expected<WindFieldOutput, core::Error>
WindFieldPass::add_to_graph(rhi::RenderGraph& graph, const weather::WindFieldParams& params,
                            glm::vec2 camera_xz) {
    if (!valid()) return fail("wind field pass: not created");
    // Created here, not in create(): the descriptor buffer refers to layout_,
    // which must not move afterwards (the pass is moved into its frame slot).
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    WindFieldOutput out;
    const glm::vec2 corner = wind_field_corner(camera_xz);
    out.rect = {corner, 1.0F / kExtentM, 0.0F};
    constexpr VkExtent2D extent{wind_field_resolution, wind_field_resolution};
    out.texture = graph.create_transient_image(
        "wind_field", wind_field_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    const WindFieldGpu push = pack_wind_field(params, corner, wind_field_texel_m);
    const rhi::ResourceHandle texture = out.texture;
    (void)graph.add_pass("wind_field", rhi::PassType::compute)
        .writes(texture, rhi::ResourceUsage::storage_write)
        .execute([this, texture, push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_storage_image(out_slot_, ctx.resolve(texture).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            constexpr std::uint32_t groups = (wind_field_resolution + 7) / 8;
            vkCmdDispatch(cmd, groups, groups, 1);
        });
    return out;
}

} // namespace engine::renderer
