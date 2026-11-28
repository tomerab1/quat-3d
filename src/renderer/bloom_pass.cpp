#include "engine/renderer/bloom_pass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/lighting_pass.hpp" // hdr_color_format
#include "engine/renderer/non_finite_counters.hpp"
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
    float         threshold, knee, prefilter;
    std::uint32_t count_non_finite; // 1: the shader counts into g_non_finite
};
static_assert(sizeof(DownParams) == 16, "must match bloom_down.slang Params");

// Each shader's resources, by name; binding slots come from its reflection
// (CLAUDE.md: never hard-code binding indices).
enum DownResource : std::uint8_t { d_src, d_sampler, d_dst, d_non_finite, d_count };
constexpr std::array<rhi::BindingRequest, d_count> kDownResources{{
    {"g_src", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_samp", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_dst", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {"g_non_finite", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};
enum UpResource : std::uint8_t { u_src, u_sampler, u_base, u_dst, u_count };
constexpr std::array<rhi::BindingRequest, u_count> kUpResources{{
    {"g_src", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_samp", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_base", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_dst", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
enum CompositeResource : std::uint8_t { c_bloom, c_sampler, c_hdr, c_count };
constexpr std::array<rhi::BindingRequest, c_count> kCompositeResources{{
    {"g_bloom", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_samp", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_hdr", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
static_assert(d_count == BloomPass::k_down_resources, "resize BloomPass::down_slots_");
static_assert(u_count == BloomPass::k_up_resources, "resize BloomPass::up_slots_");
static_assert(c_count == BloomPass::k_composite_resources, "resize BloomPass::composite_slots_");

// One pass's shader, reflected layout and slots.
template <std::size_t N>
[[nodiscard]] std::expected<const rhi::LoadedShader*, core::Error>
reflect_layout(const rhi::Device& device, const rhi::DescriptorBufferFunctions& fns,
               rhi::PipelineCache& cache, const std::string& spv,
               const std::array<rhi::BindingRequest, N>& requests,
               std::array<std::uint32_t, N>& slots, rhi::DescriptorSetLayout& layout) {
    auto shader = cache.load(spv);
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, requests, kComputeStage);
    if (!bindings) return fail("bloom: " + bindings.error().message);
    for (std::size_t i = 0; i < N; ++i) slots[i] = (*bindings)[i].binding;
    auto created = rhi::DescriptorSetLayout::create(device, fns, *bindings);
    if (!created) return std::unexpected(created.error());
    layout = std::move(*created);
    return *shader;
}

struct UpParams {
    float radius, pad0, pad1, pad2;
};
struct CompositeParams {
    float intensity, radius, pad0, pad1;
};

[[nodiscard]] std::expected<rhi::ManagedPipeline, core::Error>
make_pipeline(rhi::PipelineCache& cache, const rhi::LoadedShader* shader,
              const rhi::DescriptorSetLayout& layout, std::uint32_t push_size) {
    const VkDescriptorSetLayout set_layout = layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_size};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = shader;
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

    auto down_shader = reflect_layout(device, out.db_fns_, cache,
                                      cooked_shader_dir + "/bloom_down.spv", kDownResources,
                                      out.down_slots_, out.down_layout_);
    if (!down_shader) return std::unexpected(down_shader.error());
    auto up_shader = reflect_layout(device, out.db_fns_, cache, cooked_shader_dir + "/bloom_up.spv",
                                    kUpResources, out.up_slots_, out.up_layout_);
    if (!up_shader) return std::unexpected(up_shader.error());
    auto comp_shader = reflect_layout(device, out.db_fns_, cache,
                                      cooked_shader_dir + "/bloom_composite.spv",
                                      kCompositeResources, out.composite_slots_,
                                      out.composite_layout_);
    if (!comp_shader) return std::unexpected(comp_shader.error());

    auto down = make_pipeline(cache, *down_shader, out.down_layout_, sizeof(DownParams));
    if (!down) return std::unexpected(down.error());
    out.down_pipeline_ = std::move(*down);
    auto up = make_pipeline(cache, *up_shader, out.up_layout_, sizeof(UpParams));
    if (!up) return std::unexpected(up.error());
    out.up_pipeline_ = std::move(*up);
    auto comp = make_pipeline(cache, *comp_shader, out.composite_layout_, sizeof(CompositeParams));
    if (!comp) return std::unexpected(comp.error());
    out.composite_pipeline_ = std::move(*comp);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // A valid buffer for the counter slot when counting is off (the shader
    // does not write it then).
    auto counter = allocator.create_buffer(
        NonFiniteCounters::k_counter_range,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO);
    if (!counter) return std::unexpected(counter.error());
    out.unused_counter_ = std::move(*counter);
    out.unused_counter_address_ = rhi::buffer_device_address(device.handle(), out.unused_counter_);
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
                const auto& s = down_slots_;
                const bool count = prefilter && non_finite_counter_ != 0;
                db.write_sampled_image(s[d_src], ctx.resolve(src).view, ro);
                db.write_sampler(s[d_sampler], sampler_.handle());
                db.write_storage_image(s[d_dst], ctx.resolve(dst).view, gen);
                db.write_storage_buffer(s[d_non_finite],
                                        count ? non_finite_counter_ : unused_counter_address_,
                                        NonFiniteCounters::k_counter_range);
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_pipeline_.handle());
                db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_pipeline_.layout(), 0);
                DownParams p{params.threshold, params.knee, prefilter ? 1.0F : 0.0F,
                             count ? 1U : 0U};
                vkCmdPushConstants(cmd, down_pipeline_.layout(), kComputeStage, 0, sizeof(p), &p);
                vkCmdDispatch(cmd, groups(ds.width), groups(ds.height), 1);
                if (count) NonFiniteCounters::make_host_visible(cmd);
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
                const auto& s = up_slots_;
                db.write_sampled_image(s[u_src], ctx.resolve(src).view, ro);
                db.write_sampler(s[u_sampler], sampler_.handle());
                db.write_sampled_image(s[u_base], ctx.resolve(base).view, ro);
                db.write_storage_image(s[u_dst], ctx.resolve(dst).view, gen);
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
    auto cidx = add_db(composite_layout_);
    if (!cidx) return std::unexpected(cidx.error());
    graph.add_pass("bloom_composite", rhi::PassType::compute)
        .reads(bloom, rhi::ResourceUsage::sampled_compute)
        // Read-modify-write: the composite adds the blurred chain onto the
        // HDR texels it reads (audit A7 — write-only access left the read
        // unsynchronised).
        .writes(hdr, rhi::ResourceUsage::storage_read_write)
        .execute([this, di = *cidx, bloom, hdr, extent, params](rhi::PassContext& ctx) {
            rhi::DescriptorBuffer& db = frame_descriptors_[di];
            const auto& s = composite_slots_;
            db.write_sampled_image(s[c_bloom], ctx.resolve(bloom).view, ro);
            db.write_sampler(s[c_sampler], sampler_.handle());
            db.write_storage_image(s[c_hdr], ctx.resolve(hdr).view, gen);
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
