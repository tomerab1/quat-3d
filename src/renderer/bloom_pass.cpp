#include "engine/renderer/bloom_pass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/lighting_pass.hpp" // hdr_color_format
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t      kMaxMips      = 6;
constexpr std::uint32_t      kMinMipSize   = 8;

struct DownParams {
    float threshold, knee, prefilter, pad;
};
struct UpParams {
    float radius, pad0, pad1, pad2;
};
struct CompositeParams {
    float intensity, radius, pad0, pad1;
};

[[nodiscard]] std::expected<rhi::ManagedPipeline, core::Error>
make_pipeline(rhi::PipelineCache& cache, const std::string& spv,
              const rhi::DescriptorSetLayout& layout, std::uint32_t push_size) {
    auto shader = cache.load(spv);
    if (!shader) return std::unexpected(shader.error());
    const VkDescriptorSetLayout set_layout = layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_size};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    return cache.create_compute_pipeline(info);
}

[[nodiscard]] std::uint32_t groups(std::uint32_t n) { return (n + 7) / 8; }

} // namespace

std::expected<BloomPass, core::Error>
BloomPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    BloomPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("bloom: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 3> down_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    const std::array<DescriptorBinding, 4> up_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    auto down_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, down_b);
    if (!down_layout) return std::unexpected(down_layout.error());
    out.down_layout_ = std::move(*down_layout);
    auto up_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, up_b);
    if (!up_layout) return std::unexpected(up_layout.error());
    out.up_layout_ = std::move(*up_layout);

    auto down = make_pipeline(cache, cooked_shader_dir + "/bloom_down.spv", out.down_layout_,
                              sizeof(DownParams));
    if (!down) return std::unexpected(down.error());
    out.down_pipeline_ = std::move(*down);
    auto up = make_pipeline(cache, cooked_shader_dir + "/bloom_up.spv", out.up_layout_,
                            sizeof(UpParams));
    if (!up) return std::unexpected(up.error());
    out.up_pipeline_ = std::move(*up);
    auto comp = make_pipeline(cache, cooked_shader_dir + "/bloom_composite.spv",
                              out.down_layout_, sizeof(CompositeParams));
    if (!comp) return std::unexpected(comp.error());
    out.composite_pipeline_ = std::move(*comp);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);
    return out;
}

std::expected<void, core::Error>
BloomPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, VkExtent2D extent,
                        const BloomParams& params) {
    // Mip chain extents, starting at half resolution.
    std::vector<VkExtent2D> sizes;
    VkExtent2D s{std::max(extent.width / 2, 1u), std::max(extent.height / 2, 1u)};
    for (std::uint32_t i = 0; i < kMaxMips && std::min(s.width, s.height) >= kMinMipSize; ++i) {
        sizes.push_back(s);
        s = {std::max(s.width / 2, 1u), std::max(s.height / 2, 1u)};
    }
    if (sizes.empty()) return {}; // too small to bloom — no-op
    const std::size_t n = sizes.size();

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    std::vector<rhi::ResourceHandle> down(n), up(n);
    for (std::size_t i = 0; i < n; ++i) {
        down[i] = graph.create_transient_image("bloom_down", hdr_color_format, sizes[i], usage);
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        up[i] = graph.create_transient_image("bloom_up", hdr_color_format, sizes[i], usage);
    }

    frame_descriptors_.clear();
    frame_descriptors_.reserve(n + (n > 0 ? n - 1 : 0) + 1);
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkImageLayout gen = VK_IMAGE_LAYOUT_GENERAL;

    const auto add_db = [&](const rhi::DescriptorSetLayout& layout)
        -> std::expected<std::size_t, core::Error> {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout);
        if (!db) return std::unexpected(db.error());
        frame_descriptors_.push_back(std::move(*db));
        return frame_descriptors_.size() - 1;
    };

    // --- downsample chain ---------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        const rhi::ResourceHandle src = (i == 0) ? hdr : down[i - 1];
        const rhi::ResourceHandle dst = down[i];
        const bool prefilter = (i == 0);
        auto idx = add_db(down_layout_);
        if (!idx) return std::unexpected(idx.error());
        const VkExtent2D ds = sizes[i];
        graph.add_pass("bloom_down", rhi::PassType::compute)
            .reads(src, rhi::ResourceUsage::sampled_compute)
            .writes(dst, rhi::ResourceUsage::storage_write)
            .execute([this, di = *idx, src, dst, ds, params, prefilter](rhi::PassContext& ctx) {
                rhi::DescriptorBuffer& db = frame_descriptors_[di];
                db.write_sampled_image(0, ctx.resolve(src).view, ro);
                db.write_sampler(1, sampler_.handle());
                db.write_storage_image(2, ctx.resolve(dst).view, gen);
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_pipeline_.handle());
                db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_pipeline_.layout(), 0);
                DownParams p{params.threshold, params.knee, prefilter ? 1.0F : 0.0F, 0.0F};
                vkCmdPushConstants(cmd, down_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
                vkCmdDispatch(cmd, groups(ds.width), groups(ds.height), 1);
            });
    }

    // --- upsample chain (largest blurred level ends in up[0]) ---------------
    for (std::size_t i = n - 1; i-- > 0;) {
        const rhi::ResourceHandle src = (i + 1 == n - 1) ? down[n - 1] : up[i + 1];
        const rhi::ResourceHandle base = down[i];
        const rhi::ResourceHandle dst = up[i];
        auto idx = add_db(up_layout_);
        if (!idx) return std::unexpected(idx.error());
        const VkExtent2D us = sizes[i];
        graph.add_pass("bloom_up", rhi::PassType::compute)
            .reads(src, rhi::ResourceUsage::sampled_compute)
            .reads(base, rhi::ResourceUsage::sampled_compute)
            .writes(dst, rhi::ResourceUsage::storage_write)
            .execute([this, di = *idx, src, base, dst, us, params](rhi::PassContext& ctx) {
                rhi::DescriptorBuffer& db = frame_descriptors_[di];
                db.write_sampled_image(0, ctx.resolve(src).view, ro);
                db.write_sampler(1, sampler_.handle());
                db.write_sampled_image(2, ctx.resolve(base).view, ro);
                db.write_storage_image(3, ctx.resolve(dst).view, gen);
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, up_pipeline_.handle());
                db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, up_pipeline_.layout(), 0);
                UpParams p{params.radius, 0.0F, 0.0F, 0.0F};
                vkCmdPushConstants(cmd, up_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
                vkCmdDispatch(cmd, groups(us.width), groups(us.height), 1);
            });
    }

    // --- composite back into the full-res HDR -------------------------------
    const rhi::ResourceHandle bloom = (n >= 2) ? up[0] : down[0];
    auto cidx = add_db(down_layout_);
    if (!cidx) return std::unexpected(cidx.error());
    graph.add_pass("bloom_composite", rhi::PassType::compute)
        .reads(bloom, rhi::ResourceUsage::sampled_compute)
        // Read-modify-write: the composite adds the blurred chain onto the
        // HDR texels it reads (audit A7 — write-only access left the read
        // unsynchronised).
        .writes(hdr, rhi::ResourceUsage::storage_read_write)
        .execute([this, di = *cidx, bloom, hdr, extent, params](rhi::PassContext& ctx) {
            rhi::DescriptorBuffer& db = frame_descriptors_[di];
            db.write_sampled_image(0, ctx.resolve(bloom).view, ro);
            db.write_sampler(1, sampler_.handle());
            db.write_storage_image(2, ctx.resolve(hdr).view, gen);
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_.handle());
            db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_.layout(), 0);
            CompositeParams p{params.intensity, params.radius, 0.0F, 0.0F};
            vkCmdPushConstants(cmd, composite_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
            vkCmdDispatch(cmd, groups(extent.width), groups(extent.height), 1);
        });
    return {};
}

} // namespace engine::renderer
