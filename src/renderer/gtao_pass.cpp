#include "engine/renderer/gtao_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t      kGroupSize = 8; // numthreads of both shaders
// XeGTAO's depth MIP chain has five levels; deeper (coarser) Hi-Z levels
// would over-occlude with the nearest-depth reduction.
constexpr float kMaxDepthMip = 4.0F;

// Mirrors GtaoConstants in gtao.slang (std140, 224 bytes).
struct GtaoConstants {
    glm::mat4 view{1.0F};
    glm::mat4 inv_view{1.0F};
    glm::vec4 ndc_to_view{0.0F};
    glm::vec4 pixel{0.0F};
    glm::vec4 params0{0.0F};
    glm::vec4 params1{0.0F};
    glm::vec4 params2{0.0F};
    glm::vec4 params3{0.0F};
};
static_assert(sizeof(GtaoConstants) == 224, "must match gtao.slang GtaoConstants");

// Mirrors DenoiseParams in gtao_denoise.slang.
struct GtaoDenoisePush {
    float blur_amount = 1.2F;
    float unused = 0.0F;
};

constexpr std::array<rhi::BindingRequest, 6> kMainResources{{
    {"g_consts", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_hiz", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_normal", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_point", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_ao_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_edges_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr std::array<rhi::BindingRequest, 3> kDenoiseResources{{
    {"g_ao_in", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_edges", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_ao_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

struct Built {
    rhi::DescriptorSetLayout                layout;
    rhi::ManagedPipeline                    pipeline;
    std::vector<rhi::DescriptorBinding>     bindings;
};

template <std::size_t N>
[[nodiscard]] std::expected<Built, core::Error>
build_pipeline(const rhi::Device& device, const rhi::DescriptorBufferFunctions& fns,
               rhi::PipelineCache& cache, const std::string& spv,
               const std::array<rhi::BindingRequest, N>& requests, std::uint32_t push_bytes) {
    auto shader = cache.load(spv);
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, requests, kComputeStage);
    if (!bindings) return fail("gtao pass: " + bindings.error().message);
    Built out;
    out.bindings = std::move(*bindings);
    auto layout = rhi::DescriptorSetLayout::create(device, fns, out.bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout = std::move(*layout);
    const VkDescriptorSetLayout set_layout = out.layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_bytes};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    if (push_bytes > 0) info.push_constants = {&pc, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline = std::move(*pipeline);
    return out;
}

[[nodiscard]] GtaoConstants make_constants(const glm::mat4& view, const glm::mat4& projection,
                                           VkExtent2D extent, const GtaoSettings& s,
                                           std::uint32_t frame_index) {
    GtaoConstants c;
    c.view = view;
    c.inv_view = glm::inverse(view);
    // XeGTAO's screen-uv -> view-xy mapping (y up, screen v down).
    const float tan_x = 1.0F / projection[0][0];
    const float tan_y = 1.0F / std::abs(projection[1][1]);
    const glm::vec2 mul(tan_x * 2.0F, tan_y * -2.0F);
    const glm::vec2 add(-tan_x, tan_y);
    const glm::vec2 pixel(1.0F / static_cast<float>(extent.width),
                          1.0F / static_cast<float>(extent.height));
    c.ndc_to_view = {mul, add};
    c.pixel = {pixel, mul * pixel};
    c.params0 = {s.radius, s.falloff_range, s.sample_distribution_power,
                 s.thin_occluder_compensation};
    c.params1 = {s.final_value_power, s.depth_mip_offset, static_cast<float>(s.slice_count),
                 static_cast<float>(s.steps_per_slice)};
    c.params2 = {projection[2][2], projection[3][2], static_cast<float>(frame_index % 64U),
                 kMaxDepthMip};
    c.params3 = {s.bent_normals ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F};
    return c;
}

} // namespace

std::expected<GtaoPass, core::Error> GtaoPass::create(const rhi::Device& device,
                                                      rhi::GpuAllocator& allocator,
                                                      rhi::PipelineCache& cache,
                                                      const std::string& cooked_shader_dir) {
    GtaoPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("gtao pass: descriptor buffer functions unavailable");

    auto main = build_pipeline(device, out.db_fns_, cache, cooked_shader_dir + "/gtao.spv",
                               kMainResources, 0);
    if (!main) return std::unexpected(main.error());
    out.main_layout_ = std::move(main->layout);
    out.main_pipeline_ = std::move(main->pipeline);
    const auto& mb = main->bindings;
    out.main_slots_ = {mb[0].binding, mb[1].binding, mb[2].binding,
                       mb[3].binding, mb[4].binding, mb[5].binding};

    auto denoise = build_pipeline(device, out.db_fns_, cache,
                                  cooked_shader_dir + "/gtao_denoise.spv", kDenoiseResources,
                                  sizeof(GtaoDenoisePush));
    if (!denoise) return std::unexpected(denoise.error());
    out.denoise_layout_ = std::move(denoise->layout);
    out.denoise_pipeline_ = std::move(denoise->pipeline);
    const auto& db = denoise->bindings;
    out.denoise_slots_ = {db[0].binding, db[1].binding, db[2].binding};

    rhi::SamplerDesc point;
    point.filter = VK_FILTER_NEAREST;
    point.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto sampler = rhi::create_sampler(device.handle(), point);
    if (!sampler) return std::unexpected(sampler.error());
    out.point_sampler_ = std::move(*sampler);

    auto constants = allocator.create_buffer(
        sizeof(GtaoConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!constants) return std::unexpected(constants.error());
    out.constants_ = std::move(*constants);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
GtaoPass::add_to_graph(rhi::RenderGraph& graph, const HiZTargets& hiz, rhi::ResourceHandle normal,
                       VkExtent2D extent, const glm::mat4& view, const glm::mat4& projection,
                       const GtaoSettings& settings, std::uint32_t frame_index) {
    if (device_ == nullptr) return fail("gtao pass: not created");
    const GtaoSettings s = settings.sanitized();
    const bool perspective = projection[2][3] == -1.0F && projection[3][3] == 0.0F;
    if (!s.enabled || !perspective) return rhi::ResourceHandle{};
    if (!hiz.valid() || hiz.extent.width != extent.width || hiz.extent.height != extent.height) {
        return fail("gtao pass: needs a Hi-Z pyramid of the frame extent");
    }
    // Descriptor buffers are created on first use, once the pass sits at its
    // final address (they keep a pointer to its layouts).
    if (main_descriptor_.device_address() == 0) {
        auto d = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, main_layout_);
        if (!d) return std::unexpected(d.error());
        main_descriptor_ = std::move(*d);
        main_descriptor_.write_uniform_buffer(
            main_slots_.consts, rhi::buffer_device_address(device_->handle(), constants_),
            sizeof(GtaoConstants));
        main_descriptor_.write_sampler(main_slots_.point, point_sampler_.handle());
        for (rhi::DescriptorBuffer& dd : denoise_descriptors_) {
            auto dn =
                rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, denoise_layout_);
            if (!dn) return std::unexpected(dn.error());
            dd = std::move(*dn);
        }
    }
    const GtaoConstants c = make_constants(view, projection, extent, s, frame_index);
    std::memcpy(constants_.mapped(), &c, sizeof(c));
    if (auto f = constants_.flush(0, sizeof(c)); !f) return std::unexpected(f.error());

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const rhi::ResourceHandle raw = graph.create_transient_image("gtao_raw", gtao_format, extent,
                                                                 usage);
    const rhi::ResourceHandle edges =
        graph.create_transient_image("gtao_edges", gtao_edges_format, extent, usage);
    const std::uint32_t gx = (extent.width + kGroupSize - 1) / kGroupSize;
    const std::uint32_t gy = (extent.height + kGroupSize - 1) / kGroupSize;

    (void)graph.add_pass("gtao", rhi::PassType::compute)
        .reads(hiz.pyramid, rhi::ResourceUsage::sampled_compute)
        .reads(normal, rhi::ResourceUsage::sampled_compute)
        .writes(raw, rhi::ResourceUsage::storage_write)
        .writes(edges, rhi::ResourceUsage::storage_write)
        .execute([this, pyramid = hiz.pyramid, normal, raw, edges, gx, gy](rhi::PassContext& ctx) {
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rhi::DescriptorBuffer& d = main_descriptor_;
            d.write_sampled_image(main_slots_.hiz, ctx.resolve(pyramid).view, ro);
            d.write_sampled_image(main_slots_.normal, ctx.resolve(normal).view, ro);
            d.write_storage_image(main_slots_.ao_out, ctx.resolve(raw).view,
                                  VK_IMAGE_LAYOUT_GENERAL);
            d.write_storage_image(main_slots_.edges_out, ctx.resolve(edges).view,
                                  VK_IMAGE_LAYOUT_GENERAL);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, main_pipeline_.handle());
            d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, main_pipeline_.layout(), 0);
            vkCmdDispatch(cmd, gx, gy, 1);
        });

    rhi::ResourceHandle current = raw;
    static constexpr std::array<const char*, k_max_denoise> kNames{
        "gtao_denoise0", "gtao_denoise1", "gtao_denoise2"};
    for (std::uint32_t i = 0; i < s.denoise_passes; ++i) {
        const rhi::ResourceHandle out = graph.create_transient_image(kNames[i], gtao_format,
                                                                     extent, usage);
        const bool last = i + 1 == s.denoise_passes;
        const GtaoDenoisePush push{last ? s.denoise_blur_beta : s.denoise_blur_beta / 5.0F,
                                   0.0F};
        rhi::DescriptorBuffer* d = &denoise_descriptors_[i];
        (void)graph.add_pass(kNames[i], rhi::PassType::compute)
            .reads(current, rhi::ResourceUsage::sampled_compute)
            .reads(edges, rhi::ResourceUsage::sampled_compute)
            .writes(out, rhi::ResourceUsage::storage_write)
            .execute([this, d, in = current, edges, out, push, gx, gy](rhi::PassContext& ctx) {
                constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                d->write_sampled_image(denoise_slots_.ao_in, ctx.resolve(in).view, ro);
                d->write_sampled_image(denoise_slots_.edges, ctx.resolve(edges).view, ro);
                d->write_storage_image(denoise_slots_.ao_out, ctx.resolve(out).view,
                                       VK_IMAGE_LAYOUT_GENERAL);
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  denoise_pipeline_.handle());
                d->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoise_pipeline_.layout(), 0);
                vkCmdPushConstants(cmd, denoise_pipeline_.layout(), kComputeStage, 0,
                                   sizeof(push), &push);
                vkCmdDispatch(cmd, gx, gy, 1);
            });
        current = out;
    }
    return current;
}

} // namespace engine::renderer
