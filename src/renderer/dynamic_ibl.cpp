#include "engine/renderer/dynamic_ibl.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/atmosphere_pass.hpp" // self-test bakes through the LUT sky
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkFormat           kCubeFormat   = VK_FORMAT_R16G16B16A16_SFLOAT;

// Resolutions match the blocking IblBaker so the two paths are comparable.
constexpr std::uint32_t kEnvSize        = 256;
constexpr std::uint32_t kIrradianceSize = 16;
constexpr std::uint32_t kPrefilterSize  = 128;
constexpr std::uint32_t kBrdfSize       = 256;

// Cycle layout: each env face is baked as four 128x128 tiles (the per-texel
// cloud march makes a full 256x256 face too heavy for one frame's budget on
// slow rasterizers), then 1 irradiance convolution, then one prefiltered mip
// per step. A cycle therefore spans 30 frames at runtime.
constexpr std::uint32_t kEnvTile    = kEnvSize / 2;
constexpr std::uint32_t kTilesEdge  = kEnvSize / kEnvTile;
constexpr std::uint32_t kTilesFace  = kTilesEdge * kTilesEdge;
constexpr std::uint32_t kEnvSteps   = 6 * kTilesFace;
constexpr std::uint32_t kIrrStep    = kEnvSteps;
constexpr std::uint32_t kPreFirst   = kIrrStep + 1;
constexpr std::uint32_t kCycleSteps = kPreFirst + prefilter_mip_count;

// While nothing changes the probe idles; cloud drift alone re-triggers a cycle
// at this cadence (the deck moves metres per second against km-scale features,
// so reflections lagging by a second is imperceptible). Sun or cloud-setting
// edits start a cycle immediately.
constexpr float kDriftRefreshSeconds = 1.0F;

// Push-constant blocks mirroring the IBL shaders (see ibl_bake_env.slang).
struct BakeParams {
    glm::vec4     sun_dir{0.0F};
    glm::vec4     cloud_a{0.0F};
    glm::vec4     cloud_b{0.0F};
    std::uint32_t face_size = 0;
    std::uint32_t face_offset = 0;
    std::uint32_t tile_x = 0, tile_y = 0;
};
struct IrradianceParams {
    std::uint32_t face_size = 0;
    std::uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
};
struct PrefilterParams {
    float         roughness = 0.0F;
    std::uint32_t mip_size = 0;
    std::uint32_t pad0 = 0, pad1 = 0;
};
struct LutParams {
    std::uint32_t size = 0;
    std::uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
};

[[nodiscard]] std::expected<rhi::ComputePipeline, core::Error>
make_pipeline(const rhi::Device& device, rhi::PipelineCache& cache, const std::string& spv,
              const rhi::DescriptorSetLayout& layout, std::uint32_t push_size) {
    auto shader = cache.load(spv);
    if (!shader) return std::unexpected(shader.error());
    const VkDescriptorSetLayout set_layout = layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_size};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    return rhi::ComputePipeline::create(device, cache.handle(), info);
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, std::uint32_t mip_levels = 1,
                std::uint32_t layer_count = 1) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, layer_count};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

template <typename Record>
[[nodiscard]] std::expected<void, core::Error> submit_blocking(const rhi::Device& device,
                                                               Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pi, nullptr, &pool) != VK_SUCCESS) {
        return fail("dynamic ibl: vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult r = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(device.graphics_queue());
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return r == VK_SUCCESS ? std::expected<void, core::Error>{}
                           : fail("dynamic ibl: vkQueueSubmit2 failed");
}

[[nodiscard]] std::uint32_t groups(std::uint32_t n) { return (n + 7) / 8; }

constexpr VkImageLayout kGen = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkImageLayout kRo  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// The probe sets are sampled by the lighting compute pass and the transparent
// fragment pass — both must be in the src scope when a set leaves READ_ONLY.
constexpr VkPipelineStageFlags2 kSampleStages =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

} // namespace

std::expected<DynamicIbl, core::Error>
DynamicIbl::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                   rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                   const IblCycleParams& initial) {
    DynamicIbl out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    const VkDevice dev = device.handle();

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(dev);
    if (!out.db_fns_.valid()) return fail("dynamic ibl: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 4> env_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
    }};
    const std::array<DescriptorBinding, 3> conv_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    auto env_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, env_b);
    if (!env_layout) return std::unexpected(env_layout.error());
    out.env_layout_ = std::move(*env_layout);
    auto conv_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, conv_b);
    if (!conv_layout) return std::unexpected(conv_layout.error());
    out.conv_layout_ = std::move(*conv_layout);

    auto env_pipe = make_pipeline(device, cache, cooked_shader_dir + "/ibl_bake_env.spv",
                                  out.env_layout_, sizeof(BakeParams));
    if (!env_pipe) return std::unexpected(env_pipe.error());
    out.env_pipeline_ = std::move(*env_pipe);
    auto irr_pipe = make_pipeline(device, cache, cooked_shader_dir + "/ibl_irradiance.spv",
                                  out.conv_layout_, sizeof(IrradianceParams));
    if (!irr_pipe) return std::unexpected(irr_pipe.error());
    out.irradiance_pipeline_ = std::move(*irr_pipe);
    auto pre_pipe = make_pipeline(device, cache, cooked_shader_dir + "/ibl_prefilter.spv",
                                  out.conv_layout_, sizeof(PrefilterParams));
    if (!pre_pipe) return std::unexpected(pre_pipe.error());
    out.prefilter_pipeline_ = std::move(*pre_pipe);
    auto lut_pipe = make_pipeline(device, cache, cooked_shader_dir + "/ibl_brdf_lut.spv",
                                  out.env_layout_, sizeof(LutParams));
    if (!lut_pipe) return std::unexpected(lut_pipe.error());
    out.lut_pipeline_ = std::move(*lut_pipe);

    auto samp = rhi::create_sampler(dev, rhi::SamplerAddress::clamp_to_edge, prefilter_mip_count);
    if (!samp) return std::unexpected(samp.error());
    out.sampler_ = std::move(*samp);

    // --- images ---------------------------------------------------------------
    constexpr VkImageUsageFlags map_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    constexpr VkImageCreateFlags cube_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    auto env = allocator.create_image(kCubeFormat, {kEnvSize, kEnvSize}, map_usage, 1, 6,
                                      cube_flags);
    if (!env) return std::unexpected(env.error());
    out.env_ = std::move(*env);
    auto env_cube = rhi::create_image_view(dev, out.env_.handle(), kCubeFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_CUBE,
                                           0, 0, 6);
    auto env_store = rhi::create_image_view(dev, out.env_.handle(), kCubeFormat,
                                            VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, 6);
    if (!env_cube || !env_store) return fail("dynamic ibl: env view creation failed");
    out.env_cube_ = std::move(*env_cube);
    out.env_store_ = std::move(*env_store);

    auto brdf = allocator.create_image(kCubeFormat, {kBrdfSize, kBrdfSize}, map_usage);
    if (!brdf) return std::unexpected(brdf.error());
    out.brdf_lut_ = std::move(*brdf);
    auto brdf_view = rhi::create_image_view(dev, out.brdf_lut_.handle(), kCubeFormat);
    if (!brdf_view) return std::unexpected(brdf_view.error());
    out.brdf_view_ = std::move(*brdf_view);

    auto fb = allocator.create_image(kCubeFormat, {1, 1},
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!fb) return std::unexpected(fb.error());
    out.fallback_lut_ = std::move(*fb);
    auto fb_view = rhi::create_image_view(dev, out.fallback_lut_.handle(), kCubeFormat);
    if (!fb_view) return std::unexpected(fb_view.error());
    out.fallback_view_ = std::move(*fb_view);

    for (ProbeSet& set : out.sets_) {
        auto irr = allocator.create_image(kCubeFormat, {kIrradianceSize, kIrradianceSize},
                                          map_usage, 1, 6, cube_flags);
        if (!irr) return std::unexpected(irr.error());
        set.irradiance = std::move(*irr);
        auto pre = allocator.create_image(kCubeFormat, {kPrefilterSize, kPrefilterSize}, map_usage,
                                          prefilter_mip_count, 6, cube_flags);
        if (!pre) return std::unexpected(pre.error());
        set.prefiltered = std::move(*pre);

        auto irr_cube = rhi::create_image_view(dev, set.irradiance.handle(), kCubeFormat,
                                               VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                               VK_IMAGE_VIEW_TYPE_CUBE, 0, 0, 6);
        auto pre_cube = rhi::create_image_view(dev, set.prefiltered.handle(), kCubeFormat,
                                               VK_IMAGE_ASPECT_COLOR_BIT, prefilter_mip_count,
                                               VK_IMAGE_VIEW_TYPE_CUBE, 0, 0, 6);
        auto irr_store = rhi::create_image_view(dev, set.irradiance.handle(), kCubeFormat,
                                                VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                                VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, 6);
        if (!irr_cube || !pre_cube || !irr_store) {
            return fail("dynamic ibl: probe view creation failed");
        }
        set.irr_cube = std::move(*irr_cube);
        set.pre_cube = std::move(*pre_cube);
        set.irr_store = std::move(*irr_store);
        set.pre_store.clear();
        set.pre_store.reserve(prefilter_mip_count);
        for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
            auto v = rhi::create_image_view(dev, set.prefiltered.handle(), kCubeFormat,
                                            VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                            VK_IMAGE_VIEW_TYPE_2D_ARRAY, mip, 0, 6);
            if (!v) return fail("dynamic ibl: prefilter mip view failed");
            set.pre_store.push_back(std::move(*v));
        }
    }

    // --- one-time init: steady-state layouts + static BRDF LUT + fallback -----
    auto lut_db = rhi::DescriptorBuffer::create(device, allocator, out.db_fns_, out.env_layout_);
    if (!lut_db) return std::unexpected(lut_db.error());
    lut_db->write_storage_image(0, out.brdf_view_.handle(), kGen);
    lut_db->write_sampled_image(1, out.fallback_view_.handle(), kRo);
    lut_db->write_sampled_image(2, out.fallback_view_.handle(), kRo);
    lut_db->write_sampler(3, out.sampler_.handle());

    const VkImage env_img = out.env_.handle();
    const VkImage brdf_img = out.brdf_lut_.handle();
    const VkImage fb_img = out.fallback_lut_.handle();
    auto init = submit_blocking(device, [&](VkCommandBuffer cmd) {
        constexpr auto top = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        constexpr auto cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr auto w = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        constexpr auto r = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

        // Fallback: clear to black, leave sampled.
        VkClearColorValue black{};
        transition(cmd, fb_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   top, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, fb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transition(cmd, fb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kRo,
                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, cs, r);

        // env lives in GENERAL; the probe sets idle in READ_ONLY.
        transition(cmd, env_img, VK_IMAGE_LAYOUT_UNDEFINED, kGen, top, 0, cs, w, 1, 6);
        for (const ProbeSet& set : out.sets_) {
            transition(cmd, set.irradiance.handle(), VK_IMAGE_LAYOUT_UNDEFINED, kRo, top, 0,
                       kSampleStages, r, 1, 6);
            transition(cmd, set.prefiltered.handle(), VK_IMAGE_LAYOUT_UNDEFINED, kRo, top, 0,
                       kSampleStages, r, prefilter_mip_count, 6);
        }

        // Static BRDF LUT (sun-independent, shared by both sets).
        transition(cmd, brdf_img, VK_IMAGE_LAYOUT_UNDEFINED, kGen, top, 0, cs, w);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, out.lut_pipeline_.handle());
        lut_db->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, out.lut_pipeline_.layout(), 0);
        LutParams lp{};
        lp.size = kBrdfSize;
        vkCmdPushConstants(cmd, out.lut_pipeline_.layout(), kComputeStage, 0, sizeof(lp), &lp);
        vkCmdDispatch(cmd, groups(kBrdfSize), groups(kBrdfSize), 1);
        transition(cmd, brdf_img, kGen, kRo, cs, w, kSampleStages, r);
    });
    if (!init) return std::unexpected(init.error());

    // --- initial blocking cycle so views() is complete from frame one ---------
    out.cycle_params_ = initial;
    out.last_cycle_start_s_ = initial.clouds.time_s;
    std::vector<rhi::DescriptorBuffer> dbs;
    dbs.reserve(kCycleSteps);
    for (std::uint32_t step = 0; step < kCycleSteps; ++step) {
        auto db = out.make_step_db(step, /*back=*/1, initial);
        if (!db) return std::unexpected(db.error());
        dbs.push_back(std::move(*db));
    }
    auto baked = submit_blocking(device, [&](VkCommandBuffer cmd) {
        for (std::uint32_t step = 0; step < kCycleSteps; ++step) {
            out.record_step(cmd, step, /*back=*/1, initial, dbs[step]);
        }
    });
    if (!baked) return std::unexpected(baked.error());
    out.front_ = 1; // the cycle wrote set 1; set 0 becomes the first runtime back set

    return out;
}

IblViewSet DynamicIbl::views() const {
    const ProbeSet& set = sets_[front_];
    return {set.irr_cube.handle(), set.pre_cube.handle(), brdf_view_.handle(), sampler_.handle()};
}

std::expected<rhi::DescriptorBuffer, core::Error>
DynamicIbl::make_step_db(std::uint32_t step, std::uint32_t back_index,
                         const IblCycleParams& params) {
    const ProbeSet& back = sets_[back_index];
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_,
                                            db_fns_, step < kEnvSteps ? env_layout_ : conv_layout_);
    if (!db) return std::unexpected(db.error());
    if (step < kEnvSteps) {
        const bool atmos =
            params.skyview != VK_NULL_HANDLE && params.transmittance != VK_NULL_HANDLE;
        db->write_storage_image(0, env_store_.handle(), kGen);
        db->write_sampled_image(1, atmos ? params.skyview : fallback_view_.handle(), kRo);
        db->write_sampled_image(2, atmos ? params.transmittance : fallback_view_.handle(), kRo);
        db->write_sampler(3, sampler_.handle());
    } else {
        db->write_sampled_image(0, env_cube_.handle(), kGen); // env stays GENERAL
        db->write_sampler(1, sampler_.handle());
        const VkImageView target = step == kIrrStep
                                       ? back.irr_store.handle()
                                       : back.pre_store[step - kPreFirst].handle();
        db->write_storage_image(2, target, kGen);
    }
    return db;
}

void DynamicIbl::record_step(VkCommandBuffer cmd, std::uint32_t step, std::uint32_t back_index,
                             const IblCycleParams& params, rhi::DescriptorBuffer& db) {
    constexpr auto cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto w = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    constexpr auto r = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    const ProbeSet& back = sets_[back_index];

    if (step < kEnvSteps) {
        if (step == 0) {
            // env: last cycle's convolution reads (or init) -> this cycle's writes.
            transition(cmd, env_.handle(), kGen, kGen, cs, r | w, cs, w, 1, 6);
        }
        const bool atmos =
            params.skyview != VK_NULL_HANDLE && params.transmittance != VK_NULL_HANDLE;
        const std::uint32_t tile = step % kTilesFace;
        BakeParams bp{};
        bp.sun_dir = glm::vec4(glm::normalize(params.sun_to), atmos ? 1.0F : 0.0F);
        bp.cloud_a = params.clouds.pack_a();
        bp.cloud_b = params.clouds.pack_b();
        bp.face_size = kEnvSize;
        bp.face_offset = step / kTilesFace;
        bp.tile_x = (tile % kTilesEdge) * kEnvTile;
        bp.tile_y = (tile / kTilesEdge) * kEnvTile;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, env_pipeline_.handle());
        db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, env_pipeline_.layout(), 0);
        vkCmdPushConstants(cmd, env_pipeline_.layout(), kComputeStage, 0, sizeof(bp), &bp);
        vkCmdDispatch(cmd, groups(kEnvTile), groups(kEnvTile), 1);
        return;
    }

    if (step == kIrrStep) {
        // env: face writes -> convolution reads. The back irradiance leaves
        // READ_ONLY; prior-frame samples of it (it was the front two cycles
        // ago) are in the src scope.
        transition(cmd, env_.handle(), kGen, kGen, cs, w, cs, r, 1, 6);
        transition(cmd, back.irradiance.handle(), kRo, kGen, kSampleStages, r, cs, w, 1, 6);
        IrradianceParams ip{};
        ip.face_size = kIrradianceSize;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_pipeline_.handle());
        db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_pipeline_.layout(), 0);
        vkCmdPushConstants(cmd, irradiance_pipeline_.layout(), kComputeStage, 0, sizeof(ip), &ip);
        vkCmdDispatch(cmd, groups(kIrradianceSize), groups(kIrradianceSize), 6);
        transition(cmd, back.irradiance.handle(), kGen, kRo, cs, w, kSampleStages, r, 1, 6);
        return;
    }

    const std::uint32_t mip = step - kPreFirst;
    if (mip == 0) {
        transition(cmd, back.prefiltered.handle(), kRo, kGen, kSampleStages, r, cs, w,
                   prefilter_mip_count, 6);
    }
    PrefilterParams pp{};
    pp.roughness = static_cast<float>(mip) / static_cast<float>(prefilter_mip_count - 1);
    pp.mip_size = kPrefilterSize >> mip;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_pipeline_.handle());
    db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_pipeline_.layout(), 0);
    vkCmdPushConstants(cmd, prefilter_pipeline_.layout(), kComputeStage, 0, sizeof(pp), &pp);
    vkCmdDispatch(cmd, groups(pp.mip_size), groups(pp.mip_size), 6);
    if (mip == prefilter_mip_count - 1) {
        transition(cmd, back.prefiltered.handle(), kGen, kRo, cs, w, kSampleStages, r,
                   prefilter_mip_count, 6);
    }
}

std::expected<void, core::Error> DynamicIbl::add_step_to_graph(rhi::RenderGraph& graph,
                                                               const IblCycleParams& params) {
    if (step_ == 0) {
        // Between cycles: only start a new one when the inputs changed (sun or
        // cloud settings) or the clouds have drifted long enough to warrant a
        // refresh. Everything else leaves the probe idle — no GPU cost.
        const CloudSettings& a = params.clouds;
        const CloudSettings& b = cycle_params_.clouds;
        const bool sun_moved =
            glm::dot(glm::normalize(params.sun_to), glm::normalize(cycle_params_.sun_to)) <
            0.99999F;
        const bool clouds_edited =
            a.enabled != b.enabled || a.coverage != b.coverage || a.density != b.density ||
            a.bottom_km != b.bottom_km || a.thickness_km != b.thickness_km ||
            a.size_km != b.size_km || a.detail != b.detail || a.wind_speed != b.wind_speed ||
            a.wind_dir_deg != b.wind_dir_deg;
        const bool atmos_changed = params.skyview != cycle_params_.skyview ||
                                   params.transmittance != cycle_params_.transmittance;
        const bool drift_due =
            a.enabled && a.wind_speed > 0.0F &&
            a.time_s - last_cycle_start_s_ >= kDriftRefreshSeconds;
        if (!(sun_moved || clouds_edited || atmos_changed || drift_due)) {
            return {};
        }
        cycle_params_ = params; // latch for face consistency
        last_cycle_start_s_ = a.time_s;
    }
    const std::uint32_t step = step_;
    const std::uint32_t back = front_ ^ 1U; // latched: front_ flips below, the
                                            // lambda runs after that

    auto db = make_step_db(step, back, cycle_params_);
    if (!db) return std::unexpected(db.error());
    ring_ = (ring_ + 1) % db_ring_.size();
    db_ring_[ring_] = std::move(*db);

    auto pass = graph.add_pass("ibl_step", rhi::PassType::compute);
    pass.execute([this, step, back, slot = ring_, p = cycle_params_](rhi::PassContext& ctx) {
        record_step(ctx.cmd(), step, back, p, db_ring_[slot]);
    });

    if (++step_ == kCycleSteps) {
        step_ = 0;
        front_ ^= 1U; // this frame's consumers pick up the freshly completed set
    }
    return {};
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error>
run_dynamic_ibl_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                          rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    // Use the atmosphere LUT path: its transmittance makes a near-horizon sun
    // dim the whole sky strongly, giving the probe an unambiguous signal to
    // verify (the procedural fallback's zenith barely depends on the sun).
    auto atmos = AtmospherePass::create(device, allocator, cache, cooked_shader_dir);
    if (!atmos) return std::unexpected(atmos.error());

    // Noon probe: the initial blocking cycle must land sky-blue zenith
    // irradiance in the front set.
    IblCycleParams noon{};
    noon.sun_to = glm::normalize(glm::vec3(0.3F, 1.0F, 0.3F));
    noon.clouds.enabled = false; // keep the zenith readback deterministic
    if (auto r = atmos->ensure_skyview(noon.sun_to); !r) return std::unexpected(r.error());
    noon.skyview = atmos->skyview_view();
    noon.transmittance = atmos->transmittance_view();
    auto dyn = DynamicIbl::create(device, allocator, cache, cooked_shader_dir, noon);
    if (!dyn) return std::unexpected(dyn.error());

    constexpr std::uint32_t up_face = 2;
    const auto centre = [&](const std::vector<float>& texels) {
        const std::size_t i = (static_cast<std::size_t>(kIrradianceSize / 2) * kIrradianceSize +
                               kIrradianceSize / 2) * 4;
        return glm::vec3(texels[i], texels[i + 1], texels[i + 2]);
    };
    auto irr0 = read_rgba16f_subresource(device, allocator, dyn->front_irradiance_image(), 0,
                                         up_face, kIrradianceSize, kIrradianceSize);
    if (!irr0) return std::unexpected(irr0.error());
    const glm::vec3 noon_zenith = centre(*irr0);
    if (noon_zenith.b <= 0.0F || noon_zenith.b <= noon_zenith.r) {
        return fail("dynamic ibl self-test: initial irradiance is not sky-blue");
    }

    // Run one full incremental cycle with a near-horizon sun, one step per
    // graph as at runtime. The sets must flip and the new front must differ.
    const IblViewSet before = dyn->views();
    IblCycleParams dusk{};
    dusk.sun_to = glm::normalize(glm::vec3(0.95F, 0.12F, 0.0F));
    dusk.clouds.enabled = false;
    if (auto r = atmos->ensure_skyview(dusk.sun_to); !r) return std::unexpected(r.error());
    dusk.skyview = atmos->skyview_view();
    dusk.transmittance = atmos->transmittance_view();
    for (std::uint32_t i = 0; i < kCycleSteps; ++i) {
        rhi::RenderGraph graph;
        if (auto r = dyn->add_step_to_graph(graph, dusk); !r) return std::unexpected(r.error());
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());
        auto run = submit_blocking(device, [&](VkCommandBuffer cmd) { graph.execute(cmd); });
        if (!run) return std::unexpected(run.error());
    }
    if (dyn->views().irradiance == before.irradiance) {
        return fail("dynamic ibl self-test: probe sets did not flip after a cycle");
    }

    auto irr1 = read_rgba16f_subresource(device, allocator, dyn->front_irradiance_image(), 0,
                                         up_face, kIrradianceSize, kIrradianceSize);
    if (!irr1) return std::unexpected(irr1.error());
    const glm::vec3 dusk_zenith = centre(*irr1);
    if (!(dusk_zenith.r >= 0.0F) || !(dusk_zenith.b >= 0.0F)) {
        return fail("dynamic ibl self-test: dusk irradiance is not finite");
    }
    // The zenith must dim measurably as the sun drops to the horizon.
    if (dusk_zenith.b > noon_zenith.b * 0.8F) {
        return fail("dynamic ibl self-test: irradiance did not respond to the sun move");
    }
    return {};
}

} // namespace engine::renderer
