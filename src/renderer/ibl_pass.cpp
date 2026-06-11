#include "engine/renderer/ibl_pass.hpp"

#include <array>
#include <cstdint>
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
constexpr VkFormat           kCubeFormat   = VK_FORMAT_R16G16B16A16_SFLOAT;

// Resolutions of the precomputed maps (see ibl_pass.hpp for the mip count).
constexpr std::uint32_t kEnvSize        = 256; // environment cube face
constexpr std::uint32_t kIrradianceSize = 16;  // diffuse irradiance cube face
constexpr std::uint32_t kPrefilterSize  = 128; // prefiltered specular base face
constexpr std::uint32_t kBrdfSize       = 256; // BRDF LUT edge

// Push-constant blocks mirroring the IBL shaders.
struct BakeParams {
    glm::vec4     sun_dir{0.0F};
    glm::vec4     cloud_a{0.0F}; // CloudSettings::pack_a (LUT-sky path only)
    glm::vec4     cloud_b{0.0F}; // CloudSettings::pack_b
    std::uint32_t face_size = 0;
    std::uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
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

// Transition `image` (color aspect) between layouts with a full compute barrier.
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

// One-time command-buffer submit on the graphics queue (compute runs there too on
// our single-family devices). Blocks until the GPU finishes.
template <typename Record>
[[nodiscard]] std::expected<void, core::Error> submit_blocking(const rhi::Device& device,
                                                               Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pi, nullptr, &pool) != VK_SUCCESS) {
        return fail("ibl: vkCreateCommandPool failed");
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
                           : fail("ibl: vkQueueSubmit2 failed");
}

} // namespace

std::expected<IblBaker, core::Error>
IblBaker::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                 rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    IblBaker out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("ibl: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    // Env bake: cube storage out + the atmosphere LUTs (fallback-bound when the
    // procedural sky path is used).
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
    auto lut_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, env_b);
    if (!lut_layout) return std::unexpected(lut_layout.error());
    out.lut_layout_ = std::move(*lut_layout);

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
                                  out.lut_layout_, sizeof(LutParams));
    if (!lut_pipe) return std::unexpected(lut_pipe.error());
    out.lut_pipeline_ = std::move(*lut_pipe);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge,
                                       prefilter_mip_count);
    if (!sampler) return std::unexpected(sampler.error());
    out.env_sampler_ = std::move(*sampler);

    // 1x1 black fallback bound at the atmosphere-LUT slots when the procedural
    // sky path is used (the shader never samples them then).
    auto fb = allocator.create_image(kCubeFormat, {1, 1},
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!fb) return std::unexpected(fb.error());
    out.fallback_lut_ = std::move(*fb);
    const VkImage fb_img = out.fallback_lut_.handle();
    auto cleared = submit_blocking(device, [&](VkCommandBuffer cmd) {
        VkClearColorValue black{};
        transition(cmd, fb_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, fb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
        transition(cmd, fb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!cleared) return std::unexpected(cleared.error());
    auto fb_view = rhi::create_image_view(device.handle(), fb_img, kCubeFormat);
    if (!fb_view) return std::unexpected(fb_view.error());
    out.fallback_lut_view_ = std::move(*fb_view);
    return out;
}

std::expected<IblMaps, core::Error> IblBaker::bake(const glm::vec3& sun_dir,
                                                   VkImageView atmosphere_skyview,
                                                   VkImageView atmosphere_transmittance,
                                                   const CloudSettings& clouds) {
    const bool has_atmosphere =
        atmosphere_skyview != VK_NULL_HANDLE && atmosphere_transmittance != VK_NULL_HANDLE;
    const VkDevice dev = device_->handle();
    // TRANSFER_SRC enables debug readback (the self-test) and future mip blits.
    constexpr VkImageUsageFlags map_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    constexpr VkImageCreateFlags cube_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    // --- images -------------------------------------------------------------
    auto env = allocator_->create_image(kCubeFormat, {kEnvSize, kEnvSize}, map_usage, 1, 6,
                                        cube_flags);
    if (!env) return std::unexpected(env.error());
    IblMaps maps;
    if (auto i = allocator_->create_image(kCubeFormat, {kIrradianceSize, kIrradianceSize},
                                          map_usage, 1, 6, cube_flags);
        i) {
        maps.irradiance = std::move(*i);
    } else {
        return std::unexpected(i.error());
    }
    if (auto p = allocator_->create_image(kCubeFormat, {kPrefilterSize, kPrefilterSize}, map_usage,
                                          prefilter_mip_count, 6, cube_flags);
        p) {
        maps.prefiltered = std::move(*p);
    } else {
        return std::unexpected(p.error());
    }
    if (auto l = allocator_->create_image(kCubeFormat, {kBrdfSize, kBrdfSize}, map_usage); l) {
        maps.brdf_lut = std::move(*l);
    } else {
        return std::unexpected(l.error());
    }

    // --- views (storage = 2D array for compute writes; sampled = cube/2D) ----
    const auto array_view = [&](VkImage img, std::uint32_t base_mip) {
        return rhi::create_image_view(dev, img, kCubeFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                      VK_IMAGE_VIEW_TYPE_2D_ARRAY, base_mip, 0, 6);
    };
    auto env_store = array_view(env->handle(), 0);
    auto env_cube = rhi::create_image_view(dev, env->handle(), kCubeFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_CUBE, 0,
                                           0, 6);
    auto irr_store = array_view(maps.irradiance.handle(), 0);
    if (!env_store || !env_cube || !irr_store) return fail("ibl: view creation failed");

    std::vector<rhi::ImageView> prefilter_store; // one per mip
    prefilter_store.reserve(prefilter_mip_count);
    for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
        auto v = array_view(maps.prefiltered.handle(), mip);
        if (!v) return fail("ibl: prefilter mip view failed");
        prefilter_store.push_back(std::move(*v));
    }
    auto lut_store = rhi::create_image_view(dev, maps.brdf_lut.handle(), kCubeFormat);
    if (!lut_store) return std::unexpected(lut_store.error());

    // --- descriptor buffers (kept alive through the submit) -----------------
    std::vector<rhi::DescriptorBuffer> dbs;
    dbs.reserve(3 + prefilter_mip_count);
    const auto new_db = [&](const rhi::DescriptorSetLayout& layout)
        -> std::expected<rhi::DescriptorBuffer*, core::Error> {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout);
        if (!db) return std::unexpected(db.error());
        dbs.push_back(std::move(*db));
        return &dbs.back();
    };

    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkImageLayout gen = VK_IMAGE_LAYOUT_GENERAL;

    auto env_db = new_db(env_layout_);
    if (!env_db) return std::unexpected(env_db.error());
    (*env_db)->write_storage_image(0, env_store->handle(), gen);
    (*env_db)->write_sampled_image(
        1, has_atmosphere ? atmosphere_skyview : fallback_lut_view_.handle(), ro);
    (*env_db)->write_sampled_image(
        2, has_atmosphere ? atmosphere_transmittance : fallback_lut_view_.handle(), ro);
    (*env_db)->write_sampler(3, env_sampler_.handle());

    auto irr_db = new_db(conv_layout_);
    if (!irr_db) return std::unexpected(irr_db.error());
    (*irr_db)->write_sampled_image(0, env_cube->handle(), ro);
    (*irr_db)->write_sampler(1, env_sampler_.handle());
    (*irr_db)->write_storage_image(2, irr_store->handle(), gen);

    std::vector<rhi::DescriptorBuffer*> pre_dbs;
    pre_dbs.reserve(prefilter_mip_count);
    for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
        auto db = new_db(conv_layout_);
        if (!db) return std::unexpected(db.error());
        (*db)->write_sampled_image(0, env_cube->handle(), ro);
        (*db)->write_sampler(1, env_sampler_.handle());
        (*db)->write_storage_image(2, prefilter_store[mip].handle(), gen);
        pre_dbs.push_back(*db);
    }

    auto lut_db = new_db(lut_layout_);
    if (!lut_db) return std::unexpected(lut_db.error());
    (*lut_db)->write_storage_image(0, lut_store->handle(), gen);

    const VkImage env_img = env->handle();
    const VkImage irr_img = maps.irradiance.handle();
    const VkImage pre_img = maps.prefiltered.handle();
    const VkImage lut_img = maps.brdf_lut.handle();
    const glm::vec3 sun = sun_dir;

    auto recorded = submit_blocking(*device_, [&](VkCommandBuffer cmd) {
        constexpr auto cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr auto w = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        constexpr auto r = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        const auto dispatch = [&](const rhi::ComputePipeline& pipe, rhi::DescriptorBuffer& db,
                                  const void* push, std::uint32_t push_size, std::uint32_t gx,
                                  std::uint32_t gy, std::uint32_t gz) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.handle());
            db.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout(), 0);
            vkCmdPushConstants(cmd, pipe.layout(), kComputeStage, 0, push_size, push);
            vkCmdDispatch(cmd, gx, gy, gz);
        };
        const auto groups = [](std::uint32_t n) { return (n + 7) / 8; };

        // 1) bake env from sky
        transition(cmd, env_img, VK_IMAGE_LAYOUT_UNDEFINED, gen, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                   0, cs, w, 1, 6);
        BakeParams bp{};
        bp.sun_dir = glm::vec4(sun, has_atmosphere ? 1.0F : 0.0F);
        bp.cloud_a = clouds.pack_a();
        bp.cloud_b = clouds.pack_b();
        bp.face_size = kEnvSize;
        dispatch(env_pipeline_, **env_db, &bp, sizeof(bp), groups(kEnvSize), groups(kEnvSize), 6);

        // env: written -> sampled by the convolutions
        transition(cmd, env_img, gen, ro, cs, w, cs, r, 1, 6);

        // 2) irradiance
        transition(cmd, irr_img, VK_IMAGE_LAYOUT_UNDEFINED, gen, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                   0, cs, w, 1, 6);
        IrradianceParams ip{};
        ip.face_size = kIrradianceSize;
        dispatch(irradiance_pipeline_, **irr_db, &ip, sizeof(ip), groups(kIrradianceSize),
                 groups(kIrradianceSize), 6);

        // 3) prefiltered specular, one dispatch per mip
        transition(cmd, pre_img, VK_IMAGE_LAYOUT_UNDEFINED, gen, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                   0, cs, w, prefilter_mip_count, 6);
        for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
            PrefilterParams pp{};
            pp.roughness = static_cast<float>(mip) / static_cast<float>(prefilter_mip_count - 1);
            pp.mip_size = kPrefilterSize >> mip;
            dispatch(prefilter_pipeline_, *pre_dbs[mip], &pp, sizeof(pp), groups(pp.mip_size),
                     groups(pp.mip_size), 6);
        }

        // 4) BRDF LUT
        transition(cmd, lut_img, VK_IMAGE_LAYOUT_UNDEFINED, gen, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                   0, cs, w);
        LutParams lp{};
        lp.size = kBrdfSize;
        dispatch(lut_pipeline_, **lut_db, &lp, sizeof(lp), groups(kBrdfSize), groups(kBrdfSize), 1);

        // 5) all outputs -> sampled for the lighting pass
        transition(cmd, irr_img, gen, ro, cs, w, cs, r, 1, 6);
        transition(cmd, pre_img, gen, ro, cs, w, cs, r, prefilter_mip_count, 6);
        transition(cmd, lut_img, gen, ro, cs, w, cs, r);
    });
    if (!recorded) return std::unexpected(recorded.error());

    // --- persistent sample views --------------------------------------------
    auto irr_view = rhi::create_image_view(dev, irr_img, kCubeFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                           VK_IMAGE_VIEW_TYPE_CUBE, 0, 0, 6);
    auto pre_view = rhi::create_image_view(dev, pre_img, kCubeFormat, VK_IMAGE_ASPECT_COLOR_BIT,
                                           prefilter_mip_count, VK_IMAGE_VIEW_TYPE_CUBE, 0, 0, 6);
    auto lut_view = rhi::create_image_view(dev, lut_img, kCubeFormat);
    if (!irr_view || !pre_view || !lut_view) return fail("ibl: sample view creation failed");
    maps.irradiance_view = std::move(*irr_view);
    maps.prefiltered_view = std::move(*pre_view);
    maps.brdf_lut_view = std::move(*lut_view);
    auto samp = rhi::create_sampler(dev, rhi::SamplerAddress::clamp_to_edge, prefilter_mip_count);
    if (!samp) return std::unexpected(samp.error());
    maps.sampler = std::move(*samp);
    return maps;
}

std::expected<IblMaps, core::Error>
IblMaps::create_fallback(const rhi::Device& device, rhi::GpuAllocator& allocator) {
    const VkDevice dev = device.handle();
    constexpr VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageCreateFlags cube_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    IblMaps maps;
    auto irr = allocator.create_image(kCubeFormat, {1, 1}, usage, 1, 6, cube_flags);
    auto pre = allocator.create_image(kCubeFormat, {1, 1}, usage, 1, 6, cube_flags);
    auto lut = allocator.create_image(kCubeFormat, {1, 1}, usage);
    if (!irr || !pre || !lut) return fail("ibl fallback: image creation failed");
    maps.irradiance = std::move(*irr);
    maps.prefiltered = std::move(*pre);
    maps.brdf_lut = std::move(*lut);

    // Clear all three to black, then leave them in SHADER_READ_ONLY_OPTIMAL.
    const VkImage imgs[3] = {maps.irradiance.handle(), maps.prefiltered.handle(),
                             maps.brdf_lut.handle()};
    const std::uint32_t layers[3] = {6, 6, 1};
    auto cleared = submit_blocking(device, [&](VkCommandBuffer cmd) {
        VkClearColorValue black{};
        for (int i = 0; i < 3; ++i) {
            transition(cmd, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, layers[i]);
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers[i]};
            vkCmdClearColorImage(cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                                 &range);
            transition(cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, layers[i]);
        }
    });
    if (!cleared) return std::unexpected(cleared.error());

    auto irr_view = rhi::create_image_view(dev, maps.irradiance.handle(), kCubeFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_CUBE, 0,
                                           0, 6);
    auto pre_view = rhi::create_image_view(dev, maps.prefiltered.handle(), kCubeFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_CUBE, 0,
                                           0, 6);
    auto lut_view = rhi::create_image_view(dev, maps.brdf_lut.handle(), kCubeFormat);
    if (!irr_view || !pre_view || !lut_view) return fail("ibl fallback: view creation failed");
    maps.irradiance_view = std::move(*irr_view);
    maps.prefiltered_view = std::move(*pre_view);
    maps.brdf_lut_view = std::move(*lut_view);
    auto samp = rhi::create_sampler(dev, rhi::SamplerAddress::clamp_to_edge);
    if (!samp) return std::unexpected(samp.error());
    maps.sampler = std::move(*samp);
    return maps;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000U) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1FU;
    const std::uint32_t mant = h & 0x3FFU;
    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant != 0) {
            std::uint32_t e = 127 - 15 + 1;
            std::uint32_t m = mant;
            while ((m & 0x400U) == 0) { m <<= 1; --e; }
            m &= 0x3FFU;
            bits = sign | (e << 23) | (m << 13);
        } else {
            bits = sign;
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out = 0.0F;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Copy one (mip, layer) subresource of an RGBA16F image (currently in
// SHADER_READ_ONLY_OPTIMAL) into host floats, RGBA per texel.
[[nodiscard]] std::expected<std::vector<float>, core::Error>
read_subresource(const rhi::Device& device, rhi::GpuAllocator& allocator, VkImage image,
                 std::uint32_t mip, std::uint32_t layer, std::uint32_t w, std::uint32_t h) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 8;
    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer rb = readback->handle();
    auto done = submit_blocking(device, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);

        // Restore the sampled layout for any later use.
        std::swap(b.oldLayout, b.newLayout);
        std::swap(b.srcStageMask, b.dstStageMask);
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    if (!done) return std::unexpected(done.error());

    const auto* halfs = static_cast<const std::uint16_t*>(readback->mapped());
    std::vector<float> out(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = half_to_float(halfs[i]);
    return out;
}

} // namespace

std::expected<void, core::Error>
run_ibl_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                  rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    auto baker = IblBaker::create(device, allocator, cache, cooked_shader_dir);
    if (!baker) return std::unexpected(baker.error());
    auto maps = baker->bake(glm::normalize(glm::vec3(0.3F, 1.0F, 0.3F)));
    if (!maps) return std::unexpected(maps.error());

    // BRDF LUT: at high NdotV + low roughness the scale -> ~1, bias -> ~0; every
    // texel must be finite and in [0, ~1].
    auto lut = read_subresource(device, allocator, maps->brdf_lut.handle(), 0, 0, kBrdfSize,
                                kBrdfSize);
    if (!lut) return std::unexpected(lut.error());
    const auto lut_at = [&](std::uint32_t x, std::uint32_t y, int c) {
        return (*lut)[(static_cast<std::size_t>(y) * kBrdfSize + x) * 4 + c];
    };
    const float scale = lut_at(kBrdfSize - 4, 4, 0);
    const float bias = lut_at(kBrdfSize - 4, 4, 1);
    if (!(scale > 0.8F && scale <= 1.05F) || !(bias >= -0.01F && bias < 0.3F)) {
        return fail("ibl self-test: BRDF LUT scale/bias out of range");
    }

    // Irradiance, +Y (up) face centre: positive and blue-dominant (the sky zenith).
    constexpr std::uint32_t up_face = 2;
    auto irr = read_subresource(device, allocator, maps->irradiance.handle(), 0, up_face,
                                kIrradianceSize, kIrradianceSize);
    if (!irr) return std::unexpected(irr.error());
    const std::size_t ic = (static_cast<std::size_t>(kIrradianceSize / 2) * kIrradianceSize +
                            kIrradianceSize / 2) * 4;
    if ((*irr)[ic] <= 0.0F || (*irr)[ic + 2] <= 0.0F || (*irr)[ic + 2] <= (*irr)[ic]) {
        return fail("ibl self-test: irradiance is not positive sky-blue");
    }

    // Prefiltered mip 0 (mirror), +Y face centre: a near-mirror sky sample, also
    // positive and blue-dominant.
    auto pre = read_subresource(device, allocator, maps->prefiltered.handle(), 0, up_face,
                                kPrefilterSize, kPrefilterSize);
    if (!pre) return std::unexpected(pre.error());
    const std::size_t pc = (static_cast<std::size_t>(kPrefilterSize / 2) * kPrefilterSize +
                            kPrefilterSize / 2) * 4;
    // b >= r (the exact +Y texel may land on a white cloud, where b == r).
    if ((*pre)[pc] <= 0.0F || (*pre)[pc + 2] < (*pre)[pc]) {
        return fail("ibl self-test: prefiltered mirror is not positive sky-blue");
    }
    return {};
}

} // namespace engine::renderer
