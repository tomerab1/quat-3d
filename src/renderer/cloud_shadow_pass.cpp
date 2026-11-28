#include "engine/renderer/cloud_shadow_pass.hpp"

#include <array>
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
    {"g_cloud_shadow", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

// Mirrors CloudShadowGpu in cloud_shadow.slang (push constants, 96 bytes).
struct CloudShadowGpu {
    glm::vec4 cloud_a{0.0F};
    glm::vec4 cloud_b{0.0F};
    glm::vec4 sun{0.0F};    // xyz toward the sun, w texel size m
    glm::vec4 right{0.0F};  // xyz axis, w snapped centre coordinate
    glm::vec4 up{0.0F};
    glm::vec4 camera{0.0F}; // xyz position, w resolution
};
static_assert(sizeof(CloudShadowGpu) == 96, "must match cloud_shadow.slang CloudShadowGpu");

constexpr float kExtent = cloud_shadow_extent_m;

} // namespace

CloudShadowFrame CloudShadowFrame::make(const glm::vec3& sun_to, const glm::vec3& camera) {
    CloudShadowFrame f;
    f.camera = camera;
    glm::vec3 s = glm::length(sun_to) > 1e-6F ? glm::normalize(sun_to) : glm::vec3(0, 1, 0);
    if (s.y < cloud_shadow_min_sun_y) {
        // Hold the map at the lowest elevation it follows (no pop at dusk).
        glm::vec2 h(s.x, s.z);
        h = glm::length(h) > 1e-6F ? glm::normalize(h) : glm::vec2(1.0F, 0.0F);
        const float c = std::sqrt(1.0F - cloud_shadow_min_sun_y * cloud_shadow_min_sun_y);
        s = glm::vec3(h.x * c, cloud_shadow_min_sun_y, h.y * c);
    }
    f.sun = s;
    // Light-space basis; X as the reference near the zenith.
    const glm::vec3 ref = std::abs(s.y) < 0.99F ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    f.right = glm::normalize(glm::cross(ref, s));
    f.up = glm::cross(s, f.right);
    // Snap the centre to whole texels in light space.
    const glm::vec2 c(glm::dot(camera, f.right), glm::dot(camera, f.up));
    f.centre = glm::floor(c / f.texel_m) * f.texel_m;
    return f;
}

CloudShadowProjection CloudShadowFrame::projection() const {
    // uv = (dot(P, axis) - centre) / extent + 0.5
    CloudShadowProjection p;
    p.u_axis = {right / kExtent, 0.5F - centre.x / kExtent};
    p.v_axis = {up / kExtent, 0.5F - centre.y / kExtent};
    return p;
}

glm::vec3 CloudShadowFrame::texel_point(std::uint32_t x, std::uint32_t y) const {
    const float half_res = static_cast<float>(cloud_shadow_resolution) * 0.5F;
    const float lu = centre.x + (static_cast<float>(x) + 0.5F - half_res) * texel_m;
    const float lv = centre.y + (static_cast<float>(y) + 0.5F - half_res) * texel_m;
    const glm::vec3 on_plane = right * lu + up * lv + sun * glm::dot(camera, sun);
    return on_plane - sun * ((on_plane.y - camera.y) / sun.y);
}

std::expected<CloudShadowPass, core::Error>
CloudShadowPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    CloudShadowPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("cloud shadow pass: descriptor buffer functions unavailable");
    }
    auto shader = cache.load(cooked_shader_dir + "/cloud_shadow.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings =
        rhi::reflected_bindings((*shader)->reflection, kResources, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!bindings) return fail("cloud shadow pass: " + bindings.error().message);
    out.out_slot_ = (*bindings)[0].binding;
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudShadowGpu)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&push, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);
    return out;
}

std::expected<CloudShadowMap, core::Error>
CloudShadowPass::add_to_graph(rhi::RenderGraph& graph, const glm::vec3& sun_to,
                              const glm::vec3& camera, const CloudSettings& clouds) {
    if (!valid()) return fail("cloud shadow pass: not created");
    // Created here: the descriptor buffer refers to layout_, which must not
    // move afterwards (the pass is moved into its frame slot).
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    const CloudShadowFrame frame = CloudShadowFrame::make(sun_to, camera);
    CloudShadowMap out;
    out.projection = frame.projection();
    constexpr VkExtent2D extent{cloud_shadow_resolution, cloud_shadow_resolution};
    out.texture = graph.create_transient_image(
        "cloud_shadow", cloud_shadow_format, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    CloudShadowGpu push;
    push.cloud_a = clouds.pack_a();
    push.cloud_b = clouds.pack_b();
    push.sun = {frame.sun, frame.texel_m};
    push.right = {frame.right, frame.centre.x};
    push.up = {frame.up, frame.centre.y};
    push.camera = {camera, static_cast<float>(cloud_shadow_resolution)};

    const rhi::ResourceHandle texture = out.texture;
    (void)graph.add_pass("cloud_shadow", rhi::PassType::compute)
        .writes(texture, rhi::ResourceUsage::storage_write)
        .execute([this, texture, push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_storage_image(out_slot_, ctx.resolve(texture).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            constexpr std::uint32_t groups = (cloud_shadow_resolution + 7) / 8;
            vkCmdDispatch(cmd, groups, groups, 1);
        });
    return out;
}

} // namespace engine::renderer
