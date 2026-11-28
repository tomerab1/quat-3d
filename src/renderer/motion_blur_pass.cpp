#include "engine/renderer/motion_blur_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "post_stage.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"motion blur: " + std::move(what)});
}

using rhi::BindingRequest;
constexpr VkDescriptorType k_sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType k_storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

constexpr std::array<BindingRequest, 5> k_tiles{{{"g_velocity", k_sampled},
                                                 {"g_depth", k_sampled},
                                                 {"g_tiles", k_storage},
                                                 {"g_vectors", k_storage},
                                                 {"g_view_depth", k_storage}}};
constexpr std::array<BindingRequest, 2> k_neighbor{
    {{"g_tiles", k_sampled}, {"g_neighbor", k_storage}}};
constexpr std::array<BindingRequest, 5> k_gather{{{"g_color", k_sampled},
                                                  {"g_vectors", k_sampled},
                                                  {"g_view_depth", k_sampled},
                                                  {"g_neighbor", k_sampled},
                                                  {"g_out", k_storage}}};

// Mirrors MotionBlurParams in lib/motion_blur_common.slang.
struct MotionBlurPushConstants {
    glm::vec4 sizes{0.0F}; // xy display extent, zw render extent
    glm::vec4 blur{0.0F};  // x shutter fraction, y tile size, z samples, w frame
    glm::vec4 proj{0.0F};  // x P22, y P32, zw tile grid
    glm::vec4 depth{0.0F}; // x soft depth extent (fraction)
    glm::vec4 jitter{0.0F}; // xy velocity / depth jitter (render px)
};
static_assert(sizeof(MotionBlurPushConstants) == 80,
              "must match lib/motion_blur_common.slang MotionBlurParams");

constexpr VkFormat k_tile_format = VK_FORMAT_R16G16_SFLOAT;   // half vectors (tiles, pixels)
constexpr VkFormat k_depth_format = VK_FORMAT_R32_SFLOAT;    // per-pixel linear depth
// Depth difference (as a fraction of the nearer depth) over which the
// foreground / background classification fades.
constexpr float k_soft_depth = 0.02F;

} // namespace

struct MotionBlurPass::State {
    const rhi::Device*                device = nullptr;
    rhi::GpuAllocator*                allocator = nullptr;
    rhi::DescriptorBufferFunctions    fns{};
    std::array<post::ComputeStage, 3> stages;
    post::DescriptorPool              pool;
};

MotionBlurPass::MotionBlurPass() = default;
MotionBlurPass::~MotionBlurPass() = default;
MotionBlurPass::MotionBlurPass(MotionBlurPass&&) noexcept = default;
MotionBlurPass& MotionBlurPass::operator=(MotionBlurPass&&) noexcept = default;

std::expected<MotionBlurPass, core::Error>
MotionBlurPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    const std::array<VkFormat, 1> extended{k_tile_format};
    if (auto ok = device.require_extended_storage_formats(extended, "motion blur pass"); !ok) {
        return std::unexpected(ok.error());
    }
    auto state = std::make_unique<State>();
    state->device = &device;
    state->allocator = &allocator;
    state->fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!state->fns.valid()) return fail("descriptor buffer functions unavailable");

    struct Spec {
        const char*                     name;
        std::span<const BindingRequest> resources;
    };
    const std::array<Spec, 3> specs{{{"motion_blur_tiles", k_tiles},
                                     {"motion_blur_neighbor", k_neighbor},
                                     {"motion_blur", k_gather}}};
    for (std::size_t i = 0; i < specs.size(); ++i) {
        auto stage = post::make_compute_stage(device, state->fns, cache, cooked_shader_dir,
                                              specs[i].name, specs[i].resources,
                                              sizeof(MotionBlurPushConstants));
        if (!stage) return std::unexpected(stage.error());
        state->stages[i] = std::move(*stage);
    }
    MotionBlurPass out;
    out.state_ = std::move(state);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
MotionBlurPass::add_to_graph(rhi::RenderGraph& graph, const MotionBlurInputs& in) {
    if (!state_) return fail("pass not created");
    if (!in.color.valid() || !in.velocity.valid() || !in.depth.valid()) {
        return fail("colour, velocity and depth inputs are required");
    }
    const VkExtent2D display = in.display_extent;
    if (display.width == 0 || display.height == 0 || in.render_extent.width == 0 ||
        in.render_extent.height == 0) {
        return fail("empty display or render extent");
    }
    State& st = *state_;
    st.pool.reset();

    // The tile size is the largest blur radius (whole pixels).
    const float radius = std::isfinite(in.max_radius_px) ? in.max_radius_px : 32.0F;
    const auto tile = static_cast<std::uint32_t>(std::clamp(std::lround(radius), 8L, 64L));
    const VkExtent2D grid{(display.width + tile - 1) / tile, (display.height + tile - 1) / tile};
    const float fraction =
        std::isfinite(in.shutter_fraction) ? std::clamp(in.shutter_fraction, 0.0F, 2.0F) : 0.0F;

    MotionBlurPushConstants push{};
    push.sizes = glm::vec4(static_cast<float>(display.width), static_cast<float>(display.height),
                           static_cast<float>(in.render_extent.width),
                           static_cast<float>(in.render_extent.height));
    push.blur = glm::vec4(fraction, static_cast<float>(tile),
                          static_cast<float>(std::clamp(in.samples, 2, 64)),
                          static_cast<float>(in.frame_index % 1024U));
    push.proj = glm::vec4(in.proj22, in.proj32, static_cast<float>(grid.width),
                          static_cast<float>(grid.height));
    push.depth = glm::vec4(k_soft_depth, 0.0F, 0.0F, 0.0F);
    const bool jitter_ok = std::isfinite(in.jitter.x) && std::isfinite(in.jitter.y);
    push.jitter = glm::vec4(jitter_ok ? glm::clamp(in.jitter, -0.5F, 0.5F) : glm::vec2(0.0F),
                            0.0F, 0.0F);

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const rhi::ResourceHandle vectors =
        graph.create_transient_image("motion_blur_vectors", k_tile_format, display, usage);
    const rhi::ResourceHandle view_depth =
        graph.create_transient_image("motion_blur_depth", k_depth_format, display, usage);
    const rhi::ResourceHandle tiles =
        graph.create_transient_image("motion_blur_tiles", k_tile_format, grid, usage);
    const rhi::ResourceHandle neighbor =
        graph.create_transient_image("motion_blur_neighbor", k_tile_format, grid, usage);
    const rhi::ResourceHandle out = graph.create_transient_image(
        "motion_blur_out", k_output_format, display, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    std::array<std::size_t, 3> db{};
    for (std::size_t i = 0; i < db.size(); ++i) {
        auto idx = st.pool.add(*st.device, *st.allocator, st.fns, st.stages[i]);
        if (!idx) return std::unexpected(idx.error());
        db[i] = *idx;
    }
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkImageLayout gen = VK_IMAGE_LAYOUT_GENERAL;
    constexpr auto sampled = rhi::ResourceUsage::sampled_compute;
    constexpr auto storage = rhi::ResourceUsage::storage_write;
    State* s = &st;
    const rhi::ResourceHandle color = in.color;
    const rhi::ResourceHandle velocity = in.velocity;
    const rhi::ResourceHandle depth = in.depth;

    graph.add_pass("motion_blur_tiles", rhi::PassType::compute)
        .reads(velocity, sampled)
        .reads(depth, sampled)
        .writes(tiles, storage)
        .writes(vectors, storage)
        .writes(view_depth, storage)
        .execute([s, d = db[0], velocity, depth, tiles, vectors, view_depth, push,
                  grid](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[0];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(velocity).view, ro);
            b.write_sampled_image(stage.slots[1], ctx.resolve(depth).view, ro);
            b.write_storage_image(stage.slots[2], ctx.resolve(tiles).view, gen);
            b.write_storage_image(stage.slots[3], ctx.resolve(vectors).view, gen);
            b.write_storage_image(stage.slots[4], ctx.resolve(view_depth).view, gen);
            // One 8 x 8 workgroup per tile.
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), grid.width * 8,
                           grid.height * 8);
        });
    graph.add_pass("motion_blur_neighbor", rhi::PassType::compute)
        .reads(tiles, sampled)
        .writes(neighbor, storage)
        .execute([s, d = db[1], tiles, neighbor, push, grid](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[1];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(tiles).view, ro);
            b.write_storage_image(stage.slots[1], ctx.resolve(neighbor).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), grid.width, grid.height);
        });
    graph.add_pass("motion_blur", rhi::PassType::compute)
        .reads(color, sampled)
        .reads(vectors, sampled)
        .reads(view_depth, sampled)
        .reads(neighbor, sampled)
        .writes(out, storage)
        .execute([s, d = db[2], color, vectors, view_depth, neighbor, out, push,
                  display](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[2];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(color).view, ro);
            b.write_sampled_image(stage.slots[1], ctx.resolve(vectors).view, ro);
            b.write_sampled_image(stage.slots[2], ctx.resolve(view_depth).view, ro);
            b.write_sampled_image(stage.slots[3], ctx.resolve(neighbor).view, ro);
            b.write_storage_image(stage.slots[4], ctx.resolve(out).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), display.width,
                           display.height);
        });
    return out;
}

} // namespace engine::renderer
