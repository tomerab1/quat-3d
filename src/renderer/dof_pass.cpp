#include "engine/renderer/dof_pass.hpp"

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
    return std::unexpected(core::Error{"dof: " + std::move(what)});
}

using rhi::BindingRequest;
constexpr VkDescriptorType k_sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType k_storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

// Each stage's resources, by the names its shader declares.
constexpr std::array<BindingRequest, 4> k_prepare{{{"g_color", k_sampled},
                                                   {"g_depth", k_sampled},
                                                   {"g_half", k_storage},
                                                   {"g_coc", k_storage}}};
constexpr std::array<BindingRequest, 2> k_tiles{{{"g_half", k_sampled}, {"g_tiles", k_storage}}};
constexpr std::array<BindingRequest, 2> k_dilate{
    {{"g_tiles", k_sampled}, {"g_dilated", k_storage}}};
constexpr std::array<BindingRequest, 4> k_gather{{{"g_half", k_sampled},
                                                  {"g_dilated", k_sampled},
                                                  {"g_near", k_storage},
                                                  {"g_far", k_storage}}};
constexpr std::array<BindingRequest, 6> k_composite{{{"g_color", k_sampled},
                                                     {"g_coc", k_sampled},
                                                     {"g_near", k_sampled},
                                                     {"g_far", k_sampled},
                                                     {"g_linear", VK_DESCRIPTOR_TYPE_SAMPLER},
                                                     {"g_out", k_storage}}};

// Mirrors DofParams in lib/dof_common.slang.
struct DofPushConstants {
    glm::vec4 sizes{0.0F};     // xy display extent, zw render extent
    glm::vec4 coc{0.0F};       // x scale, y bias (display px), z max radius, w rings
    glm::vec4 proj{0.0F};      // x P22, y P32, zw tile grid
    glm::vec4 half_size{0.0F}; // xy half extent, zw 1 / half extent
    glm::vec4 jitter{0.0F};    // xy depth jitter (render px)
};
static_assert(sizeof(DofPushConstants) == 80, "must match lib/dof_common.slang DofParams");

constexpr std::uint32_t k_tile = 8; // half-resolution pixels per tile (DOF_TILE)
constexpr VkFormat      k_half_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat      k_tile_format = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat      k_coc_format = VK_FORMAT_R16_SFLOAT; // display-pixel CoC

} // namespace

struct DofPass::State {
    const rhi::Device*             device = nullptr;
    rhi::GpuAllocator*             allocator = nullptr;
    rhi::DescriptorBufferFunctions fns{};
    std::array<post::ComputeStage, 5> stages;
    post::DescriptorPool           pool;
    rhi::Sampler                   sampler;
};

DofPass::DofPass() = default;
DofPass::~DofPass() = default;
DofPass::DofPass(DofPass&&) noexcept = default;
DofPass& DofPass::operator=(DofPass&&) noexcept = default;

std::expected<DofPass, core::Error> DofPass::create(const rhi::Device& device,
                                                    rhi::GpuAllocator& allocator,
                                                    rhi::PipelineCache& cache,
                                                    const std::string& cooked_shader_dir) {
    const std::array<VkFormat, 2> extended{k_tile_format, k_coc_format};
    if (auto ok = device.require_extended_storage_formats(extended, "dof pass"); !ok) {
        return std::unexpected(ok.error());
    }
    auto state = std::make_unique<State>();
    state->device = &device;
    state->allocator = &allocator;
    state->fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!state->fns.valid()) return fail("descriptor buffer functions unavailable");

    struct Spec {
        const char*                    name;
        std::span<const BindingRequest> resources;
    };
    const std::array<Spec, 5> specs{{{"dof_prepare", k_prepare},
                                     {"dof_tiles", k_tiles},
                                     {"dof_dilate", k_dilate},
                                     {"dof_gather", k_gather},
                                     {"dof_composite", k_composite}}};
    for (std::size_t i = 0; i < specs.size(); ++i) {
        auto stage = post::make_compute_stage(device, state->fns, cache, cooked_shader_dir,
                                              specs[i].name, specs[i].resources,
                                              sizeof(DofPushConstants));
        if (!stage) return std::unexpected(stage.error());
        state->stages[i] = std::move(*stage);
    }
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    state->sampler = std::move(*sampler);

    DofPass out;
    out.state_ = std::move(state);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error> DofPass::add_to_graph(rhi::RenderGraph& graph,
                                                                      const DofInputs& in) {
    if (!state_) return fail("pass not created");
    if (!in.color.valid() || !in.depth.valid()) {
        return fail("colour and depth inputs are required");
    }
    const VkExtent2D display = in.display_extent;
    if (display.width == 0 || display.height == 0 || in.render_extent.width == 0 ||
        in.render_extent.height == 0) {
        return fail("empty display or render extent");
    }
    State& st = *state_;
    st.pool.reset();

    const VkExtent2D half{(display.width + 1) / 2, (display.height + 1) / 2};
    const VkExtent2D grid{(half.width + k_tile - 1) / k_tile, (half.height + k_tile - 1) / k_tile};
    DofPushConstants push{};
    push.sizes = glm::vec4(static_cast<float>(display.width), static_cast<float>(display.height),
                           static_cast<float>(in.render_extent.width),
                           static_cast<float>(in.render_extent.height));
    const float max_radius = std::isfinite(in.max_radius_px)
                                 ? std::clamp(in.max_radius_px, 1.0F, 64.0F)
                                 : 24.0F;
    const bool finite = std::isfinite(in.coc.scale) && std::isfinite(in.coc.bias);
    push.coc = glm::vec4(finite ? in.coc.scale : 0.0F, finite ? in.coc.bias : 0.0F, max_radius,
                         static_cast<float>(std::clamp(in.rings, 1, 8)));
    push.proj = glm::vec4(in.proj22, in.proj32, static_cast<float>(grid.width),
                          static_cast<float>(grid.height));
    push.half_size = glm::vec4(static_cast<float>(half.width), static_cast<float>(half.height),
                               1.0F / static_cast<float>(half.width),
                               1.0F / static_cast<float>(half.height));
    const bool jitter_ok = std::isfinite(in.jitter.x) && std::isfinite(in.jitter.y);
    push.jitter = glm::vec4(jitter_ok ? glm::clamp(in.jitter, -0.5F, 0.5F) : glm::vec2(0.0F),
                            0.0F, 0.0F);

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const rhi::ResourceHandle half_img =
        graph.create_transient_image("dof_half", k_half_format, half, usage);
    const rhi::ResourceHandle coc =
        graph.create_transient_image("dof_coc", k_coc_format, display, usage);
    const rhi::ResourceHandle tiles =
        graph.create_transient_image("dof_tiles", k_tile_format, grid, usage);
    const rhi::ResourceHandle dilated =
        graph.create_transient_image("dof_dilated", k_tile_format, grid, usage);
    const rhi::ResourceHandle near_img =
        graph.create_transient_image("dof_near", k_half_format, half, usage);
    const rhi::ResourceHandle far_img =
        graph.create_transient_image("dof_far", k_half_format, half, usage);
    const rhi::ResourceHandle out = graph.create_transient_image(
        "dof_out", k_output_format, display, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    std::array<std::size_t, 5> db{};
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
    const rhi::ResourceHandle depth = in.depth;

    graph.add_pass("dof_prepare", rhi::PassType::compute)
        .reads(color, sampled)
        .reads(depth, sampled)
        .writes(half_img, storage)
        .writes(coc, storage)
        .execute([s, d = db[0], color, depth, half_img, coc, push, half](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[0];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(color).view, ro);
            b.write_sampled_image(stage.slots[1], ctx.resolve(depth).view, ro);
            b.write_storage_image(stage.slots[2], ctx.resolve(half_img).view, gen);
            b.write_storage_image(stage.slots[3], ctx.resolve(coc).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), half.width, half.height);
        });
    graph.add_pass("dof_tiles", rhi::PassType::compute)
        .reads(half_img, sampled)
        .writes(tiles, storage)
        .execute([s, d = db[1], half_img, tiles, push, grid](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[1];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(half_img).view, ro);
            b.write_storage_image(stage.slots[1], ctx.resolve(tiles).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), grid.width, grid.height);
        });
    graph.add_pass("dof_dilate", rhi::PassType::compute)
        .reads(tiles, sampled)
        .writes(dilated, storage)
        .execute([s, d = db[2], tiles, dilated, push, grid](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[2];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(tiles).view, ro);
            b.write_storage_image(stage.slots[1], ctx.resolve(dilated).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), grid.width, grid.height);
        });
    graph.add_pass("dof_gather", rhi::PassType::compute)
        .reads(half_img, sampled)
        .reads(dilated, sampled)
        .writes(near_img, storage)
        .writes(far_img, storage)
        .execute([s, d = db[3], half_img, dilated, near_img, far_img, push,
                  half](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[3];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(half_img).view, ro);
            b.write_sampled_image(stage.slots[1], ctx.resolve(dilated).view, ro);
            b.write_storage_image(stage.slots[2], ctx.resolve(near_img).view, gen);
            b.write_storage_image(stage.slots[3], ctx.resolve(far_img).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), half.width, half.height);
        });
    graph.add_pass("dof_composite", rhi::PassType::compute)
        .reads(color, sampled)
        .reads(coc, sampled)
        .reads(near_img, sampled)
        .reads(far_img, sampled)
        .writes(out, storage)
        .execute([s, d = db[4], color, coc, near_img, far_img, out, push,
                  display](rhi::PassContext& ctx) {
            const post::ComputeStage& stage = s->stages[4];
            rhi::DescriptorBuffer& b = s->pool[d];
            b.write_sampled_image(stage.slots[0], ctx.resolve(color).view, ro);
            b.write_sampled_image(stage.slots[1], ctx.resolve(coc).view, ro);
            b.write_sampled_image(stage.slots[2], ctx.resolve(near_img).view, ro);
            b.write_sampled_image(stage.slots[3], ctx.resolve(far_img).view, ro);
            b.write_sampler(stage.slots[4], s->sampler.handle());
            b.write_storage_image(stage.slots[5], ctx.resolve(out).view, gen);
            post::dispatch(ctx.cmd(), stage, b, &push, sizeof(push), display.width,
                           display.height);
        });
    return out;
}

} // namespace engine::renderer
