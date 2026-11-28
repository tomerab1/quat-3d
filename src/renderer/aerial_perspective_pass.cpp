#include "engine/renderer/aerial_perspective_pass.hpp"

#include <utility>

#include "compute_kernel.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

enum Slot : std::uint8_t { s_transmittance, s_multiscatter, s_sampler, s_out };
constexpr std::array<rhi::BindingRequest, 4> k_bindings{{
    {"g_transmittance", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_multiscatter", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_ap", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

// Mirrors ApPush in atmos_aerial.slang (push constants, 96 bytes).
struct ApPush {
    glm::mat4 inv_view_proj{1.0F};
    glm::vec4 sun{0.0F};
    glm::vec4 moon{0.0F};
};
static_assert(sizeof(ApPush) == 96, "must match atmos_aerial.slang ApPush");

constexpr VkFormat k_format = VK_FORMAT_R16G16B16A16_SFLOAT;

} // namespace

std::expected<AerialPerspectivePass, core::Error>
AerialPerspectivePass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                              rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    AerialPerspectivePass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("aerial perspective: descriptor buffers unavailable");
    auto kernel = detail::build_compute_kernel(device, out.db_fns_, cache, cooked_shader_dir,
                                               "atmos_aerial", k_bindings, sizeof(ApPush),
                                               "aerial perspective");
    if (!kernel) return std::unexpected(kernel.error());
    out.layout_ = std::move(kernel->layout);
    out.pipeline_ = std::move(kernel->pipeline);
    for (std::size_t i = 0; i < k_bindings.size(); ++i) out.slots_[i] = kernel->slots[i];

    auto image = allocator.create_image_3d(
        k_format, {aerial_perspective_size, aerial_perspective_size, aerial_perspective_slices},
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image) return std::unexpected(image.error());
    out.volume_ = std::move(*image);
    auto view = rhi::create_image_view(device.handle(), out.volume_.handle(), k_format,
                                       VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_3D);
    if (!view) return std::unexpected(view.error());
    out.volume_view_ = std::move(*view);
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
AerialPerspectivePass::add_to_graph(rhi::RenderGraph& graph, const AerialPerspectiveInputs& in) {
    if (!valid()) return fail("aerial perspective: not created");
    if (in.transmittance == VK_NULL_HANDLE || in.multiscatter == VK_NULL_HANDLE) {
        return fail("aerial perspective: atmosphere LUTs required");
    }
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    frame_descriptor_.write_sampled_image(slots_[s_transmittance], in.transmittance, ro);
    frame_descriptor_.write_sampled_image(slots_[s_multiscatter], in.multiscatter, ro);
    frame_descriptor_.write_sampler(slots_[s_sampler], sampler_.handle());

    // Rewritten in full every frame, by this frame slot only: no history.
    const rhi::ResourceHandle volume = graph.import_image(
        "aerial_perspective", volume_.handle(), volume_view_.handle(), k_format,
        {aerial_perspective_size, aerial_perspective_size}, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED);
    ApPush push;
    push.inv_view_proj = in.inv_view_proj;
    push.sun = {in.sun_to, in.sun_irradiance};
    push.moon = {in.moon_to, in.moon_irradiance};
    (void)graph.add_pass("aerial_perspective", rhi::PassType::compute)
        .writes(volume, rhi::ResourceUsage::storage_write)
        .execute([this, volume, push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_storage_image(slots_[s_out], ctx.resolve(volume).view,
                                                  VK_IMAGE_LAYOUT_GENERAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            const std::uint32_t groups = detail::groups_of(aerial_perspective_size);
            vkCmdDispatch(cmd, groups, groups, 1);
        });
    return volume;
}

} // namespace engine::renderer
