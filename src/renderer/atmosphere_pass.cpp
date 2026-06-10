#include "engine/renderer/atmosphere_pass.hpp"

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
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

constexpr std::uint32_t kTransmittanceW = 256;
constexpr std::uint32_t kTransmittanceH = 64;
constexpr std::uint32_t kMultiScatterSize = 32;
constexpr std::uint32_t kSkyViewW = 192;
constexpr std::uint32_t kSkyViewH = 108;

struct SkyViewPush {
    float sun_direction[4];
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
    if (push_size > 0) info.push_constants = {&pc, 1};
    return rhi::ComputePipeline::create(device, cache.handle(), info);
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
        return fail("atmosphere: vkCreateCommandPool failed");
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
                           : fail("atmosphere: vkQueueSubmit2 failed");
}

} // namespace

std::expected<AtmospherePass, core::Error>
AtmospherePass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    AtmospherePass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("atmosphere: descriptor buffer functions unavailable");
    }

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 1> lut_b{
        {{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage}}};
    const std::array<DescriptorBinding, 3> ms_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    const std::array<DescriptorBinding, 4> sky_b{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};

    auto trans_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, lut_b);
    if (!trans_layout) return std::unexpected(trans_layout.error());
    auto ms_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, ms_b);
    if (!ms_layout) return std::unexpected(ms_layout.error());
    auto sky_layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, sky_b);
    if (!sky_layout) return std::unexpected(sky_layout.error());

    auto trans_pipe = make_pipeline(device, cache, cooked_shader_dir + "/atmos_transmittance.spv",
                                    *trans_layout, 0);
    if (!trans_pipe) return std::unexpected(trans_pipe.error());
    auto ms_pipe = make_pipeline(device, cache, cooked_shader_dir + "/atmos_multiscatter.spv",
                                 *ms_layout, 0);
    if (!ms_pipe) return std::unexpected(ms_pipe.error());
    auto sky_pipe = make_pipeline(device, cache, cooked_shader_dir + "/atmos_skyview.spv",
                                  *sky_layout, sizeof(SkyViewPush));
    if (!sky_pipe) return std::unexpected(sky_pipe.error());

    constexpr VkImageUsageFlags lut_usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto trans_img =
        allocator.create_image(kLutFormat, {kTransmittanceW, kTransmittanceH}, lut_usage);
    if (!trans_img) return std::unexpected(trans_img.error());
    auto ms_img =
        allocator.create_image(kLutFormat, {kMultiScatterSize, kMultiScatterSize}, lut_usage);
    if (!ms_img) return std::unexpected(ms_img.error());
    auto sky_img = allocator.create_image(kLutFormat, {kSkyViewW, kSkyViewH}, lut_usage);
    if (!sky_img) return std::unexpected(sky_img.error());
    out.transmittance_ = std::move(*trans_img);
    out.multiscatter_ = std::move(*ms_img);
    out.skyview_ = std::move(*sky_img);

    const VkDevice dev = device.handle();
    auto tv = rhi::create_image_view(dev, out.transmittance_.handle(), kLutFormat);
    if (!tv) return std::unexpected(tv.error());
    out.transmittance_view_ = std::move(*tv);
    auto mv = rhi::create_image_view(dev, out.multiscatter_.handle(), kLutFormat);
    if (!mv) return std::unexpected(mv.error());
    out.multiscatter_view_ = std::move(*mv);
    auto sv = rhi::create_image_view(dev, out.skyview_.handle(), kLutFormat);
    if (!sv) return std::unexpected(sv.error());
    out.skyview_view_ = std::move(*sv);

    auto sampler = rhi::create_sampler(dev, rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // Static LUTs: transmittance, then multiple scattering (reads it). The
    // descriptor buffers live only inside this scope — written, used in one
    // blocking submit, discarded — so the buffers never dangle when `out`
    // moves (DescriptorBuffer keeps pointers into its creator).
    auto trans_db = rhi::DescriptorBuffer::create(device, allocator, out.db_fns_, *trans_layout);
    if (!trans_db) return std::unexpected(trans_db.error());
    auto ms_db = rhi::DescriptorBuffer::create(device, allocator, out.db_fns_, *ms_layout);
    if (!ms_db) return std::unexpected(ms_db.error());

    constexpr VkImageLayout gen = VK_IMAGE_LAYOUT_GENERAL;
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    trans_db->write_storage_image(0, out.transmittance_view_.handle(), gen);
    ms_db->write_sampled_image(0, out.transmittance_view_.handle(), ro);
    ms_db->write_sampler(1, out.sampler_.handle());
    ms_db->write_storage_image(2, out.multiscatter_view_.handle(), gen);

    const VkImage trans_image = out.transmittance_.handle();
    const VkImage ms_image = out.multiscatter_.handle();
    const VkImage sky_image = out.skyview_.handle();
    auto baked = submit_blocking(device, [&](VkCommandBuffer cmd) {
        transition(cmd, trans_image, VK_IMAGE_LAYOUT_UNDEFINED, gen,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trans_pipe->handle());
        trans_db->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trans_pipe->layout(), 0);
        vkCmdDispatch(cmd, (kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8, 1);

        transition(cmd, trans_image, gen, ro, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        transition(cmd, ms_image, VK_IMAGE_LAYOUT_UNDEFINED, gen,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ms_pipe->handle());
        ms_db->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ms_pipe->layout(), 0);
        vkCmdDispatch(cmd, (kMultiScatterSize + 7) / 8, (kMultiScatterSize + 7) / 8, 1);

        transition(cmd, ms_image, gen, ro, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        // The sky-view image starts in SHADER_READ_ONLY (black is fine until
        // the first ensure_skyview) so the lighting pass can always bind it.
        transition(cmd, sky_image, VK_IMAGE_LAYOUT_UNDEFINED, ro,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!baked) return std::unexpected(baked.error());

    out.skyview_layout_ = std::move(*sky_layout);
    out.skyview_pipeline_ = std::move(*sky_pipe);
    return out;
}

std::expected<void, core::Error> AtmospherePass::ensure_skyview(const glm::vec3& sun_to) {
    const glm::vec3 sun = glm::normalize(sun_to);
    if (glm::dot(sun, skyview_sun_) > 0.99999F) return {}; // unchanged

    // Fresh descriptor buffer per recompute: sun changes are user-driven and
    // rare, and a local buffer cannot dangle across moves of this pass.
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, skyview_layout_);
    if (!db) return std::unexpected(db.error());
    constexpr VkImageLayout gen = VK_IMAGE_LAYOUT_GENERAL;
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    db->write_sampled_image(0, transmittance_view_.handle(), ro);
    db->write_sampled_image(1, multiscatter_view_.handle(), ro);
    db->write_sampler(2, sampler_.handle());
    db->write_storage_image(3, skyview_view_.handle(), gen);

    SkyViewPush push{};
    push.sun_direction[0] = sun.x;
    push.sun_direction[1] = sun.y;
    push.sun_direction[2] = sun.z;
    push.sun_direction[3] = 0.0F;

    const VkImage sky_image = skyview_.handle();
    auto submitted = submit_blocking(*device_, [&](VkCommandBuffer cmd) {
        transition(cmd, sky_image, ro, gen, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyview_pipeline_.handle());
        db->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyview_pipeline_.layout(), 0);
        vkCmdPushConstants(cmd, skyview_pipeline_.layout(), kComputeStage, 0, sizeof(push),
                           &push);
        vkCmdDispatch(cmd, (kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8, 1);
        transition(cmd, sky_image, gen, ro, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!submitted) return std::unexpected(submitted.error());

    skyview_sun_ = sun;
    return {};
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error>
run_atmosphere_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    auto pass = AtmospherePass::create(device, allocator, cache, cooked_shader_dir);
    if (!pass) return std::unexpected(pass.error());
    if (auto r = pass->ensure_skyview(glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F))); !r) {
        return std::unexpected(r.error());
    }

    // Read both LUTs back through a host buffer.
    const auto read_image = [&](VkImage image, std::uint32_t w,
                                std::uint32_t h) -> std::expected<std::vector<float>, core::Error> {
        const VkDeviceSize bytes = VkDeviceSize(w) * h * 8; // RGBA16F
        auto buf = allocator.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_AUTO,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!buf) return std::unexpected(buf.error());
        auto copied = submit_blocking(device, [&](VkCommandBuffer cmd) {
            transition(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   buf->handle(), 1, &region);
            transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!copied) return std::unexpected(copied.error());

        // Half -> float, RGBA.
        const auto* halfs = static_cast<const std::uint16_t*>(buf->mapped());
        std::vector<float> out(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const std::uint16_t v = halfs[i];
            const std::uint32_t sign = (v & 0x8000U) << 16;
            const std::uint32_t exp = (v >> 10) & 0x1FU;
            const std::uint32_t mant = v & 0x3FFU;
            std::uint32_t bits = sign;
            if (exp != 0) bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
            float f = 0.0F;
            std::memcpy(&f, &bits, sizeof(f));
            out[i] = f;
        }
        return out;
    };

    // Transmittance (y=0 row is ground level; x=0 is the zenith ray, x=max is
    // the horizon ray): the zenith is clearer than the horizon, and the
    // horizon ray is reddened — blue scatters out over the long path.
    auto trans = read_image(pass->transmittance_image(), kTransmittanceW, kTransmittanceH);
    if (!trans) return std::unexpected(trans.error());
    const auto trans_px = [&](std::uint32_t x, std::uint32_t y, int c) {
        return (*trans)[(std::size_t(y) * kTransmittanceW + x) * 4 + std::size_t(c)];
    };
    const float zen_b = trans_px(0, 0, 2);
    const float hor_r = trans_px(kTransmittanceW - 1, 0, 0);
    const float hor_b = trans_px(kTransmittanceW - 1, 0, 2);
    if (!(zen_b > 0.2F && zen_b <= 1.0F)) {
        return fail("atmosphere self-test: zenith transmittance out of range");
    }
    if (!(zen_b > hor_b && hor_r > hor_b)) {
        return fail("atmosphere self-test: horizon ray not reddened vs zenith");
    }

    // Sky-view (high sun): the upper (sky) half must be non-black and
    // blue-dominant; the lower (ground) half darker than the sky.
    auto sky = read_image(pass->skyview_image(), kSkyViewW, kSkyViewH);
    if (!sky) return std::unexpected(sky.error());
    const auto sky_px = [&](std::uint32_t x, std::uint32_t y, int c) {
        return (*sky)[(std::size_t(y) * kSkyViewW + x) * 4 + std::size_t(c)];
    };
    const std::uint32_t cx = kSkyViewW / 2;
    const float up_r = sky_px(cx, kSkyViewH / 8, 0);
    const float up_b = sky_px(cx, kSkyViewH / 8, 2);
    const float down_b = sky_px(cx, kSkyViewH - 2, 2);
    if (!(up_b > 1e-4F && up_b > up_r)) {
        return fail("atmosphere self-test: sky-view zenith not blue-dominant");
    }
    if (!(down_b < up_b)) {
        return fail("atmosphere self-test: below-horizon brighter than the sky");
    }
    return {};
}

} // namespace engine::renderer
