#include "engine/renderer/ssr_pass.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "compute_kernel.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

using detail::ComputeKernel;
using detail::groups_of;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"ssr pass: " + std::move(what)});
}

constexpr VkFormat k_hits_format = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkImageLayout k_ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout k_general = VK_IMAGE_LAYOUT_GENERAL;
// Temporal history weight (the resolve is noisy; most of the smoothing is
// temporal, the neighbourhood clamp bounds the ghosting).
constexpr float k_feedback = 0.9F;

// Mirrors SsrConstants in lib/ssr_common.slang (std140, 192 bytes).
struct SsrConstants {
    glm::mat4 view_proj{1.0F};
    glm::mat4 inv_view_proj{1.0F};
    glm::vec4 camera_pos{0.0F};
    glm::vec4 screen{0.0F};
    glm::vec4 params{0.0F};
    glm::vec4 params2{0.0F};
};
static_assert(sizeof(SsrConstants) == 192, "must match ssr_common.slang SsrConstants");

enum TraceSlot : std::uint8_t { t_consts, t_hiz, t_depth, t_normal, t_material, t_hits };
constexpr std::array<rhi::BindingRequest, 6> k_trace{{
    {"g_consts", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_hiz", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_normal", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_material", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_hits", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
enum ResolveSlot : std::uint8_t {
    r_consts, r_hits, r_depth, r_normal, r_material, r_velocity, r_color, r_linear, r_out
};
constexpr std::array<rhi::BindingRequest, 9> k_resolve{{
    {"g_consts", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_hits", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_depth", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_normal", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_material", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_velocity", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_color", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_linear", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
enum TemporalSlot : std::uint8_t { m_consts, m_current, m_history, m_velocity, m_linear, m_out };
constexpr std::array<rhi::BindingRequest, 6> k_temporal{{
    {"g_consts", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_current", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_history", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_velocity", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_linear", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

// A persistent image and what the last recorded frame left it in; the next
// frame imports it in exactly that state. Every frame ends with it in
// SHADER_READ_ONLY_OPTIMAL (the graph's final transition when the last use
// left it elsewhere), so that state is known when the frame is recorded.
struct Persistent {
    rhi::GpuImage         image;
    rhi::ImageView        view;
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = 0;

    [[nodiscard]] rhi::ResourceHandle import(rhi::RenderGraph& graph, const char* name,
                                             VkExtent2D extent) const {
        return graph.import_image(name, image.handle(), view.handle(), ssr_format, extent, layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, access);
    }
    // After a recorded frame: read-only, last touched by any earlier command
    // (the next first access needs an execution dependency on it; the
    // frame's own barriers made its writes available).
    void recorded() {
        layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = 0;
    }
};

struct FrameSlot {
    rhi::GpuBuffer        constants;
    VkDeviceAddress       address = 0;
    rhi::DescriptorBuffer trace, resolve, temporal;
};

} // namespace

struct SsrPass::Impl {
    const rhi::Device*             device = nullptr;
    rhi::GpuAllocator*             allocator = nullptr;
    rhi::DescriptorBufferFunctions fns{};
    ComputeKernel                  trace, resolve, temporal;
    rhi::Sampler                   linear;
    std::vector<FrameSlot>         slots;

    std::array<Persistent, 2> history;
    Persistent                color;
    VkExtent2D                extent{0, 0};
    std::uint64_t             frame = 0; // temporal ping-pong
    bool                      history_valid = false;
    bool                      color_valid = false;
    // This frame's colour import, for add_color_copy (valid in that graph only).
    std::optional<rhi::ResourceHandle> color_handle;

    [[nodiscard]] std::expected<void, core::Error> ensure(VkExtent2D e);
};

SsrPass::SsrPass() = default;
SsrPass::~SsrPass() = default;
SsrPass::SsrPass(SsrPass&&) noexcept = default;
SsrPass& SsrPass::operator=(SsrPass&&) noexcept = default;

std::expected<SsrPass, core::Error> SsrPass::create(const rhi::Device& device,
                                                    rhi::GpuAllocator& allocator,
                                                    rhi::PipelineCache& cache,
                                                    const std::string& cooked_shader_dir,
                                                    std::uint32_t frames_in_flight) {
    SsrPass out;
    out.impl_ = std::make_unique<Impl>();
    Impl& s = *out.impl_;
    s.device = &device;
    s.allocator = &allocator;
    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("descriptor buffer functions unavailable");
    const auto build = [&](ComputeKernel& k, const char* shader,
                           std::span<const rhi::BindingRequest> req) -> std::expected<void, core::Error> {
        auto built = detail::build_compute_kernel(device, s.fns, cache, cooked_shader_dir, shader,
                                                  req, 0, "ssr pass");
        if (!built) return std::unexpected(built.error());
        k = std::move(*built);
        return {};
    };
    if (auto r = build(s.trace, "ssr_trace", k_trace); !r) return std::unexpected(r.error());
    if (auto r = build(s.resolve, "ssr_resolve", k_resolve); !r) return std::unexpected(r.error());
    if (auto r = build(s.temporal, "ssr_temporal", k_temporal); !r) {
        return std::unexpected(r.error());
    }
    auto linear = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!linear) return std::unexpected(linear.error());
    s.linear = std::move(*linear);

    for (std::uint32_t i = 0; i < std::max(frames_in_flight, 1U); ++i) {
        FrameSlot slot;
        auto constants = allocator.create_buffer(
            sizeof(SsrConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!constants) return std::unexpected(constants.error());
        slot.constants = std::move(*constants);
        slot.address = rhi::buffer_device_address(device.handle(), slot.constants);
        s.slots.push_back(std::move(slot));
    }
    // Descriptors keep a pointer to their layout: create them once the
    // kernels sit at their final address (inside the heap-allocated Impl).
    for (FrameSlot& slot : s.slots) {
        auto t = detail::make_descriptor(device, allocator, s.fns, s.trace);
        auto r = detail::make_descriptor(device, allocator, s.fns, s.resolve);
        auto m = detail::make_descriptor(device, allocator, s.fns, s.temporal);
        if (!t || !r || !m) return fail("descriptor creation failed");
        slot.trace = std::move(*t);
        slot.resolve = std::move(*r);
        slot.temporal = std::move(*m);
        slot.trace.write_uniform_buffer(s.trace.slots[t_consts], slot.address,
                                        sizeof(SsrConstants));
        slot.resolve.write_uniform_buffer(s.resolve.slots[r_consts], slot.address,
                                          sizeof(SsrConstants));
        slot.resolve.write_sampler(s.resolve.slots[r_linear], s.linear.handle());
        slot.temporal.write_uniform_buffer(s.temporal.slots[m_consts], slot.address,
                                           sizeof(SsrConstants));
        slot.temporal.write_sampler(s.temporal.slots[m_linear], s.linear.handle());
    }
    return out;
}

std::expected<void, core::Error> SsrPass::Impl::ensure(VkExtent2D e) {
    if (extent.width == e.width && extent.height == e.height &&
        color.image.handle() != VK_NULL_HANDLE) {
        return {};
    }
    // Frames in flight may still use the old images (a render-scale change
    // is a rare, user-driven event).
    if (color.image.handle() != VK_NULL_HANDLE) vkDeviceWaitIdle(device->handle());
    const auto make = [&](Persistent& p, VkImageUsageFlags usage) -> std::expected<void, core::Error> {
        auto img = allocator->create_image(ssr_format, e, usage);
        if (!img) return std::unexpected(img.error());
        auto view = rhi::create_image_view(device->handle(), img->handle(), ssr_format);
        if (!view) return std::unexpected(view.error());
        p = Persistent{std::move(*img), std::move(*view)};
        return {};
    };
    for (Persistent& h : history) {
        if (auto r = make(h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT); !r) {
            return r;
        }
    }
    if (auto r = make(color, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT); !r) {
        return r;
    }
    extent = e;
    history_valid = false;
    color_valid = false;
    return {};
}

std::expected<rhi::ResourceHandle, core::Error>
SsrPass::add_to_graph(rhi::RenderGraph& graph, std::uint32_t slot_index, const SsrInputs& in,
                      const SsrSettings& settings) {
    if (!impl_) return fail("not created");
    Impl& s = *impl_;
    s.color_handle.reset();
    const SsrSettings st = settings.sanitized();
    if (!st.enabled || !in.hiz.valid() || !in.velocity.valid() || in.extent.width == 0 ||
        in.extent.height == 0) {
        s.history_valid = false;
        s.color_valid = false;
        return rhi::ResourceHandle{};
    }
    if (slot_index >= s.slots.size()) return fail("frame slot out of range");
    if (in.hiz.extent.width != in.extent.width || in.hiz.extent.height != in.extent.height) {
        return fail("needs a Hi-Z pyramid of the frame extent");
    }
    if (auto r = s.ensure(in.extent); !r) return std::unexpected(r.error());
    if (in.reset) {
        s.history_valid = false;
        s.color_valid = false;
    }
    FrameSlot& slot = s.slots[slot_index];
    const SsrBudget budget = ssr_budget(st.quality);
    const VkExtent2D ext = in.extent;
    SsrConstants c;
    c.view_proj = in.view_proj;
    c.inv_view_proj = glm::inverse(in.view_proj);
    c.camera_pos = {in.camera_pos, static_cast<float>(in.frame_index % 4096U)};
    c.screen = {static_cast<float>(ext.width), static_cast<float>(ext.height),
                1.0F / static_cast<float>(ext.width), 1.0F / static_cast<float>(ext.height)};
    c.params = {st.max_roughness, st.thickness, st.edge_fade,
                static_cast<float>(budget.max_steps)};
    c.params2 = {static_cast<float>(in.hiz.mip_count), s.color_valid ? 1.0F : 0.0F,
                 s.history_valid ? k_feedback : 0.0F, static_cast<float>(budget.reuse)};
    std::memcpy(slot.constants.mapped(), &c, sizeof(c));
    if (auto f = slot.constants.flush(0, sizeof(c)); !f) return std::unexpected(f.error());

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const rhi::ResourceHandle hits = graph.create_transient_image("ssr_hits", k_hits_format, ext,
                                                                  usage);
    const rhi::ResourceHandle resolved =
        graph.create_transient_image("ssr_resolved", ssr_format, ext, usage);
    Persistent& cur = s.history[s.frame % 2];
    Persistent& prev = s.history[(s.frame + 1) % 2];
    const rhi::ResourceHandle h_cur = cur.import(graph, "ssr_history_cur", ext);
    const rhi::ResourceHandle h_prev = prev.import(graph, "ssr_history_prev", ext);
    const rhi::ResourceHandle color = s.color.import(graph, "ssr_color", ext);
    s.color_handle = color;

    const std::uint32_t gx = groups_of(ext.width);
    const std::uint32_t gy = groups_of(ext.height);
    Impl* impl = &s;
    FrameSlot* fs = &slot;
    const SsrInputs inputs = in;
    graph.add_pass("ssr_trace", rhi::PassType::compute)
        .reads(in.hiz.pyramid, rhi::ResourceUsage::sampled_compute)
        .reads(in.depth, rhi::ResourceUsage::sampled_compute)
        .reads(in.normal, rhi::ResourceUsage::sampled_compute)
        .reads(in.material, rhi::ResourceUsage::sampled_compute)
        .writes(hits, rhi::ResourceUsage::storage_write)
        .execute([impl, fs, inputs, hits, gx, gy](rhi::PassContext& ctx) {
            const auto& k = impl->trace.slots;
            rhi::DescriptorBuffer& d = fs->trace;
            d.write_sampled_image(k[t_hiz], ctx.resolve(inputs.hiz.pyramid).view, k_ro);
            d.write_sampled_image(k[t_depth], ctx.resolve(inputs.depth).view, k_ro);
            d.write_sampled_image(k[t_normal], ctx.resolve(inputs.normal).view, k_ro);
            d.write_sampled_image(k[t_material], ctx.resolve(inputs.material).view, k_ro);
            d.write_storage_image(k[t_hits], ctx.resolve(hits).view, k_general);
            impl->trace.dispatch(ctx.cmd(), d, nullptr, gx, gy, 1);
        });
    graph.add_pass("ssr_resolve", rhi::PassType::compute)
        .reads(hits, rhi::ResourceUsage::sampled_compute)
        .reads(in.depth, rhi::ResourceUsage::sampled_compute)
        .reads(in.normal, rhi::ResourceUsage::sampled_compute)
        .reads(in.material, rhi::ResourceUsage::sampled_compute)
        .reads(in.velocity, rhi::ResourceUsage::sampled_compute)
        .reads(color, rhi::ResourceUsage::sampled_compute)
        .writes(resolved, rhi::ResourceUsage::storage_write)
        .execute([impl, fs, inputs, hits, color, resolved, gx, gy](rhi::PassContext& ctx) {
            const auto& k = impl->resolve.slots;
            rhi::DescriptorBuffer& d = fs->resolve;
            d.write_sampled_image(k[r_hits], ctx.resolve(hits).view, k_ro);
            d.write_sampled_image(k[r_depth], ctx.resolve(inputs.depth).view, k_ro);
            d.write_sampled_image(k[r_normal], ctx.resolve(inputs.normal).view, k_ro);
            d.write_sampled_image(k[r_material], ctx.resolve(inputs.material).view, k_ro);
            d.write_sampled_image(k[r_velocity], ctx.resolve(inputs.velocity).view, k_ro);
            d.write_sampled_image(k[r_color], ctx.resolve(color).view, k_ro);
            d.write_storage_image(k[r_out], ctx.resolve(resolved).view, k_general);
            impl->resolve.dispatch(ctx.cmd(), d, nullptr, gx, gy, 1);
        });
    graph.add_pass("ssr_temporal", rhi::PassType::compute)
        .reads(resolved, rhi::ResourceUsage::sampled_compute)
        .reads(h_prev, rhi::ResourceUsage::sampled_compute)
        .reads(in.velocity, rhi::ResourceUsage::sampled_compute)
        .writes(h_cur, rhi::ResourceUsage::storage_write)
        .execute([impl, fs, inputs, resolved, h_prev, h_cur, gx, gy](rhi::PassContext& ctx) {
            const auto& k = impl->temporal.slots;
            rhi::DescriptorBuffer& d = fs->temporal;
            d.write_sampled_image(k[m_current], ctx.resolve(resolved).view, k_ro);
            d.write_sampled_image(k[m_history], ctx.resolve(h_prev).view, k_ro);
            d.write_sampled_image(k[m_velocity], ctx.resolve(inputs.velocity).view, k_ro);
            d.write_storage_image(k[m_out], ctx.resolve(h_cur).view, k_general);
            impl->temporal.dispatch(ctx.cmd(), d, nullptr, gx, gy, 1);
        });

    cur.recorded();
    prev.recorded();
    s.color.recorded();
    s.history_valid = true;
    s.color_valid = false; // until add_color_copy refreshes it
    ++s.frame;
    return h_cur;
}

void SsrPass::add_color_copy(rhi::RenderGraph& graph, rhi::ResourceHandle hdr) {
    if (!impl_ || !impl_->color_handle || !hdr.valid()) return;
    Impl& s = *impl_;
    const rhi::ResourceHandle color = *s.color_handle;
    const VkExtent2D ext = s.extent;
    graph.add_pass("ssr_color_copy", rhi::PassType::transfer)
        .reads(hdr, rhi::ResourceUsage::transfer_src)
        .writes(color, rhi::ResourceUsage::transfer_dst)
        .execute([hdr, color, ext](rhi::PassContext& ctx) {
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {ext.width, ext.height, 1};
            vkCmdCopyImage(ctx.cmd(), ctx.resolve(hdr).image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ctx.resolve(color).image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
        });
    s.color_valid = true;
    s.color_handle.reset();
}

void SsrPass::reset() {
    if (!impl_) return;
    impl_->history_valid = false;
    impl_->color_valid = false;
}

} // namespace engine::renderer
