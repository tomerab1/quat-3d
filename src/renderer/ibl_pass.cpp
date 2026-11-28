#include "engine/renderer/ibl_pass.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/cloud_reference.hpp"
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
constexpr std::uint32_t kIrradianceSize = ibl_irradiance_size; // diffuse irradiance cube face
constexpr std::uint32_t kPrefilterSize  = ibl_prefilter_size;  // prefiltered specular base face
constexpr std::uint32_t kBrdfSize       = ibl_brdf_lut_size;   // BRDF LUT edge

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
    const std::array<DescriptorBinding, 10> env_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        // R13.4 cloud layer + noise (bindings 4..9 of ibl_bake_env.slang).
        {0, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kComputeStage},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 9, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
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

    auto env_pipe = make_pipeline(cache, cooked_shader_dir + "/ibl_bake_env.spv",
                                  out.env_layout_, sizeof(BakeParams));
    if (!env_pipe) return std::unexpected(env_pipe.error());
    out.env_pipeline_ = std::move(*env_pipe);
    auto irr_pipe = make_pipeline(cache, cooked_shader_dir + "/ibl_irradiance.spv",
                                  out.conv_layout_, sizeof(IrradianceParams));
    if (!irr_pipe) return std::unexpected(irr_pipe.error());
    out.irradiance_pipeline_ = std::move(*irr_pipe);
    auto pre_pipe = make_pipeline(cache, cooked_shader_dir + "/ibl_prefilter.spv",
                                  out.conv_layout_, sizeof(PrefilterParams));
    if (!pre_pipe) return std::unexpected(pre_pipe.error());
    out.prefilter_pipeline_ = std::move(*pre_pipe);
    auto lut_pipe = make_pipeline(cache, cooked_shader_dir + "/ibl_brdf_lut.spv",
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
    auto fb_volume = allocator.create_image_3d(
        kCubeFormat, {1, 1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!fb_volume) return std::unexpected(fb_volume.error());
    out.fallback_volume_ = std::move(*fb_volume);
    const VkImage fb_volume_img = out.fallback_volume_.handle();
    auto zero_layer = allocator.create_buffer(
        sizeof(CloudLayerGpu),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!zero_layer) return std::unexpected(zero_layer.error());
    out.zero_layer_ = std::move(*zero_layer);
    std::memset(out.zero_layer_.mapped(), 0, sizeof(CloudLayerGpu));
    if (auto f = out.zero_layer_.flush(); !f) return std::unexpected(f.error());
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
        transition(cmd, fb_volume_img, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, fb_volume_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
        transition(cmd, fb_volume_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!cleared) return std::unexpected(cleared.error());
    auto fb_view = rhi::create_image_view(device.handle(), fb_img, kCubeFormat);
    if (!fb_view) return std::unexpected(fb_view.error());
    out.fallback_lut_view_ = std::move(*fb_view);
    auto fb_volume_view = rhi::create_image_view(device.handle(), fb_volume_img, kCubeFormat,
                                                 VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                                 VK_IMAGE_VIEW_TYPE_3D);
    if (!fb_volume_view) return std::unexpected(fb_volume_view.error());
    out.fallback_volume_view_ = std::move(*fb_volume_view);
    return out;
}

std::expected<IblMaps, core::Error> IblBaker::bake(const glm::vec3& sun_dir,
                                                   VkImageView atmosphere_skyview,
                                                   VkImageView atmosphere_transmittance,
                                                   const CloudSettings& /*clouds*/) {
    const bool has_atmosphere =
        atmosphere_skyview != VK_NULL_HANDLE && atmosphere_transmittance != VK_NULL_HANDLE;
    const VkDevice dev = device_->handle();
    // TRANSFER_SRC enables debug readback (tests) and future mip blits.
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
    (*env_db)->write_uniform_buffer(4, rhi::buffer_device_address(dev, zero_layer_),
                                    sizeof(CloudLayerGpu));
    (*env_db)->write_sampled_image(5, fallback_volume_view_.handle(), ro);
    (*env_db)->write_sampled_image(6, fallback_volume_view_.handle(), ro);
    (*env_db)->write_sampled_image(7, fallback_lut_view_.handle(), ro);
    (*env_db)->write_sampled_image(8, fallback_lut_view_.handle(), ro);
    (*env_db)->write_sampler(9, env_sampler_.handle());

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
        const auto dispatch = [&](const rhi::ManagedPipeline& pipe, rhi::DescriptorBuffer& db,
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
        // No clouds (cloud_a.x = 0 steps): see bake()'s note.
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

} // namespace engine::renderer
