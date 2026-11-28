#include "engine/renderer/tonemap_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "engine/renderer/color_grading.hpp"
#include "engine/renderer/color_lut.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kFragment = VK_SHADER_STAGE_FRAGMENT_BIT;

// tonemap.slang's resources, by name; binding slots come from its reflection.
enum Resource : std::uint8_t { r_hdr, r_sampler, r_exposure, r_post, r_lut, r_count };
constexpr std::array<rhi::BindingRequest, r_count> kResources{{
    {"g_hdr", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_exposure", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_post", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_lut", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
}};

// Fragment entry point of each operator (TonemapOperator order).
constexpr std::array<const char*, tonemap_operator_names.size()> kEntries{
    "fragment_aces", "fragment_agx", "fragment_pbr_neutral"};

// Mirrors PostParams in tonemap.slang.
struct PostParams {
    glm::vec4           exposure{0.0F};  // x read the buffer, y 1 / pre-exposure
    glm::vec4           exposure2{0.0F}; // x scale
    grading::GradingGpu grading{};
    glm::vec4           vignette{0.0F};  // x natural, y artistic, z smoothness, w aspect
    glm::vec4           lens{0.0F};      // xy tan(fov / 2), z chromatic aberration
    glm::vec4           grain{0.0F};     // x intensity, y size, z seed
    glm::vec4           lut{0.0F};       // x contribution, y size
    glm::vec4           lut_domain{0.0F};
    glm::vec4           lut_scale{1.0F};
    glm::vec4           extent{0.0F};
};
static_assert(sizeof(PostParams) == 240, "must match tonemap.slang PostParams");

// Grain pattern rate: like film, a new grain field 24 times a second.
constexpr float k_grain_rate_hz = 24.0F;

[[nodiscard]] float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0F, 1.0F) : 0.0F; }

[[nodiscard]] PostParams make_params(VkExtent2D extent, bool auto_exposure,
                                     const ExposureSettings& exposure, const TonemapLook& look) {
    PostParams p{};
    const bool physical = look.physical_exposure > 0.0F && std::isfinite(look.physical_exposure);
    const float pre = std::isfinite(look.pre_exposure) && look.pre_exposure > 0.0F
                          ? look.pre_exposure
                          : 1.0F;
    const bool from_buffer = auto_exposure && !physical;
    // Auto: the shader multiplies by the buffer's exposure over the
    // pre-exposure (exposure.y); fixed exposures divide it out here. The
    // pre-exposure is divided out exactly once either way.
    p.exposure = glm::vec4(from_buffer ? 1.0F : 0.0F, 1.0F / pre, 0.0F, 0.0F);
    p.exposure2.x = std::exp2(exposure.compensation_ev) *
                    (physical ? look.physical_exposure : 1.0F) / (from_buffer ? 1.0F : pre);
    p.grading = grading::make_grading_gpu(look.grading);

    const float w = static_cast<float>(std::max(extent.width, 1U));
    const float h = static_cast<float>(std::max(extent.height, 1U));
    const float aspect = w / h;
    const float fov = std::isfinite(look.fov_y) ? std::clamp(look.fov_y, 1e-3F, 3.1F) : 1.0471976F;
    const float tan_v = std::tan(0.5F * fov);
    const FilmSettings& film = look.film;
    p.vignette = glm::vec4(clamp01(film.vignette_natural), clamp01(film.vignette_intensity),
                           std::clamp(film.vignette_smoothness, 0.05F, 1.0F), aspect);
    p.lens = glm::vec4(tan_v * aspect, tan_v, clamp01(film.chromatic_aberration), 0.0F);
    const float seed = std::floor(std::fmod(std::max(look.time_s, 0.0F) * k_grain_rate_hz, 4096.0F));
    p.grain = glm::vec4(clamp01(film.grain_intensity), std::clamp(film.grain_size_px, 0.5F, 4.0F),
                        seed, 0.0F);
    if (look.lut != nullptr && look.lut->size() >= 2) {
        p.lut = glm::vec4(clamp01(look.grading.lut_contribution),
                          static_cast<float>(look.lut->size()), 0.0F, 0.0F);
        for (int k = 0; k < 3; ++k) {
            const float lo = look.lut->domain_min()[static_cast<std::size_t>(k)];
            const float hi = look.lut->domain_max()[static_cast<std::size_t>(k)];
            p.lut_domain[k] = lo;
            p.lut_scale[k] = 1.0F / (hi - lo);
        }
    }
    p.extent = glm::vec4(w, h, 1.0F / w, 1.0F / h);
    return p;
}

} // namespace

std::expected<TonemapPass, core::Error>
TonemapPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                    VkFormat output_format) {
    TonemapPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("tonemap pass: descriptor buffer functions unavailable");
    }

    auto shader = cache.load(cooked_shader_dir + "/tonemap.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kFragment);
    if (!bindings) return fail("tonemap pass: " + bindings.error().message);
    for (const rhi::DescriptorBinding& b : *bindings) out.slots_.push_back(b.binding);
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const std::array<VkFormat, 1> color_formats{output_format};
    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    for (std::size_t i = 0; i < kEntries.size(); ++i) {
        rhi::GraphicsPipeline::CreateInfo info{};
        info.vertex = *shader;
        info.fragment = *shader;
        info.fragment_entry = kEntries[i];
        info.color_formats = color_formats; // fullscreen triangle, no vertex input
        info.cull_mode = VK_CULL_MODE_NONE;
        info.set_layouts = {&set_layout, 1};
        auto pipeline = cache.create_graphics_pipeline(info);
        if (!pipeline) return std::unexpected(pipeline.error());
        out.pipelines_[i] = std::move(*pipeline);
    }

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    const auto host_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage) {
        return allocator.create_buffer(
            size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
    };
    // Fallback exposure state (ExposureState layout, exposure 1) bound when
    // auto-exposure is off.
    auto buf = host_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!buf) return std::unexpected(buf.error());
    const std::array<float, 4> unit{1.0F, 1.0F, 0.0F, 0.18F};
    std::memcpy(buf->mapped(), unit.data(), sizeof(unit));
    if (auto flushed = buf->flush(); !flushed) return std::unexpected(flushed.error());
    out.fallback_exposure_buffer_ = std::move(*buf);
    out.fallback_exposure_address_ =
        rhi::buffer_device_address(device.handle(), out.fallback_exposure_buffer_);

    auto fallback_lut =
        allocator.create_image_3d(color_lut_format, VkExtent3D{1, 1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fallback_lut) return std::unexpected(fallback_lut.error());
    auto fallback_view =
        rhi::create_image_view(device.handle(), fallback_lut->handle(), color_lut_format,
                               VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_3D);
    if (!fallback_view) return std::unexpected(fallback_view.error());
    out.fallback_lut_ = std::move(*fallback_lut);
    out.fallback_lut_view_ = std::move(*fallback_view);

    auto params = host_buffer(sizeof(PostParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!params) return std::unexpected(params.error());
    out.params_ = std::move(*params);
    out.params_address_ = rhi::buffer_device_address(device.handle(), out.params_);
    return out;
}

std::expected<void, core::Error>
TonemapPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr,
                          rhi::ResourceHandle output, VkExtent2D extent,
                          VkDeviceAddress exposure_buffer, const ExposureSettings& exposure,
                          const TonemapLook& look) {
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    const PostParams params =
        make_params(extent, exposure_buffer != 0 && exposure.auto_exposure, exposure, look);
    std::memcpy(params_.mapped(), &params, sizeof(params));
    if (auto flushed = params_.flush(); !flushed) return std::unexpected(flushed.error());

    const VkDeviceAddress exposure_addr =
        exposure_buffer != 0 ? exposure_buffer : fallback_exposure_address_;
    const auto op_index = static_cast<std::size_t>(
        std::clamp(static_cast<int>(look.op), 0, static_cast<int>(pipelines_.size()) - 1));
    // Without a LUT the Texture3D slot still needs a valid 3D image: the
    // 1x1x1 fallback, imported with an undefined layout every frame (its
    // contents are never read), so the graph transitions it for sampling.
    VkImageView lut_view = params.lut.x > 0.0F ? look.lut->view() : VK_NULL_HANDLE;
    rhi::ResourceHandle fallback{};
    if (lut_view == VK_NULL_HANDLE) {
        fallback = graph.import_image("tonemap_fallback_lut", fallback_lut_.handle(),
                                      fallback_lut_view_.handle(), color_lut_format,
                                      VkExtent2D{1, 1}, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        lut_view = fallback_lut_view_.handle();
    }

    auto pass = graph.add_pass("tonemap", rhi::PassType::graphics);
    pass.reads(hdr, rhi::ResourceUsage::sampled)
        .writes(output, rhi::ResourceUsage::color_attachment);
    if (fallback.valid()) pass.reads(fallback, rhi::ResourceUsage::sampled);
    pass.execute([this, hdr, output, extent, exposure_addr, op_index,
                  lut_view](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const std::vector<std::uint32_t>& s = slots_;
            const VkImageView hdr_view = ctx.resolve(hdr).view;
            frame_descriptor_.write_sampled_image(s[r_hdr], hdr_view, ro);
            frame_descriptor_.write_sampler(s[r_sampler], sampler_.handle());
            frame_descriptor_.write_storage_buffer(s[r_exposure], exposure_addr, 4 * sizeof(float));
            frame_descriptor_.write_uniform_buffer(s[r_post], params_address_, sizeof(PostParams));
            frame_descriptor_.write_sampled_image(s[r_lut], lut_view, ro);

            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = ctx.resolve(output).view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;

            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            const rhi::ManagedPipeline& pipeline = pipelines_[op_index];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        });

    return {};
}

} // namespace engine::renderer
