#include "engine/renderer/cloud_noise_pass.hpp"

#include <array>
#include <utility>

#include "compute_kernel.hpp"
#include "engine/renderer/cloud_noise.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

struct BakePush {
    std::uint32_t which = 0; // volume / texture index
    std::uint32_t size = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};

constexpr std::array<rhi::BindingRequest, 1> k_volume_bindings{{
    {"g_volume", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr std::array<rhi::BindingRequest, 1> k_texture_bindings{{
    {"g_texture", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

constexpr VkImageUsageFlags k_usage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

} // namespace

std::expected<CloudNoisePass, core::Error>
CloudNoisePass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    const rhi::DescriptorBufferFunctions fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!fns.valid()) {
        return std::unexpected(core::Error{"cloud noise: descriptor buffer functions unavailable"});
    }
    auto volume_kernel =
        detail::build_compute_kernel(device, fns, cache, cooked_shader_dir, "cloud_noise_bake",
                                     k_volume_bindings, sizeof(BakePush), "cloud noise");
    if (!volume_kernel) return std::unexpected(volume_kernel.error());
    auto texture_kernel =
        detail::build_compute_kernel(device, fns, cache, cooked_shader_dir, "cloud_noise_bake_2d",
                                     k_texture_bindings, sizeof(BakePush), "cloud noise");
    if (!texture_kernel) return std::unexpected(texture_kernel.error());

    CloudNoisePass out;
    using cloud_noise::k_base_size;
    using cloud_noise::k_curl_size;
    using cloud_noise::k_detail_size;
    using cloud_noise::k_weather_size;
    auto base = allocator.create_image_3d(cloud_noise_format,
                                          {k_base_size, k_base_size, k_base_size}, k_usage);
    if (!base) return std::unexpected(base.error());
    auto detail = allocator.create_image_3d(
        cloud_noise_format, {k_detail_size, k_detail_size, k_detail_size}, k_usage);
    if (!detail) return std::unexpected(detail.error());
    auto curl = allocator.create_image(cloud_noise_format, {k_curl_size, k_curl_size}, k_usage);
    if (!curl) return std::unexpected(curl.error());
    auto weather =
        allocator.create_image(cloud_noise_format, {k_weather_size, k_weather_size}, k_usage);
    if (!weather) return std::unexpected(weather.error());
    out.base_ = std::move(*base);
    out.detail_ = std::move(*detail);
    out.curl_ = std::move(*curl);
    out.weather_ = std::move(*weather);

    const VkDevice dev = device.handle();
    auto bv = rhi::create_image_view(dev, out.base_.handle(), cloud_noise_format,
                                     VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_3D);
    if (!bv) return std::unexpected(bv.error());
    auto dv = rhi::create_image_view(dev, out.detail_.handle(), cloud_noise_format,
                                     VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_3D);
    if (!dv) return std::unexpected(dv.error());
    auto cv = rhi::create_image_view(dev, out.curl_.handle(), cloud_noise_format);
    if (!cv) return std::unexpected(cv.error());
    auto wv = rhi::create_image_view(dev, out.weather_.handle(), cloud_noise_format);
    if (!wv) return std::unexpected(wv.error());
    out.base_view_ = std::move(*bv);
    out.detail_view_ = std::move(*dv);
    out.curl_view_ = std::move(*cv);
    out.weather_view_ = std::move(*wv);
    auto sampler = rhi::create_sampler(dev, rhi::SamplerAddress::repeat);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // One descriptor buffer per dispatch: all four are recorded into one
    // command buffer, so none may be rewritten while it is pending.
    struct Job {
        const detail::ComputeKernel* kernel;
        VkImage                      image;
        VkImageView                  view;
        BakePush                     push;
        std::array<std::uint32_t, 3> groups;
    };
    const std::array<Job, 4> jobs{{
        {&*volume_kernel, out.base_.handle(), out.base_view_.handle(), {0, k_base_size, 0, 0},
         {detail::groups_of(k_base_size, 4), detail::groups_of(k_base_size, 4),
          detail::groups_of(k_base_size, 4)}},
        {&*volume_kernel, out.detail_.handle(), out.detail_view_.handle(),
         {1, k_detail_size, 0, 0},
         {detail::groups_of(k_detail_size, 4), detail::groups_of(k_detail_size, 4),
          detail::groups_of(k_detail_size, 4)}},
        {&*texture_kernel, out.curl_.handle(), out.curl_view_.handle(), {0, k_curl_size, 0, 0},
         {detail::groups_of(k_curl_size), detail::groups_of(k_curl_size), 1}},
        {&*texture_kernel, out.weather_.handle(), out.weather_view_.handle(),
         {1, k_weather_size, 0, 0},
         {detail::groups_of(k_weather_size), detail::groups_of(k_weather_size), 1}},
    }};
    std::array<rhi::DescriptorBuffer, 4> sets;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        auto db = detail::make_descriptor(device, allocator, fns, *jobs[i].kernel);
        if (!db) return std::unexpected(db.error());
        sets[i] = std::move(*db);
        sets[i].write_storage_image(jobs[i].kernel->slots[0], jobs[i].view,
                                    VK_IMAGE_LAYOUT_GENERAL);
    }
    auto baked = detail::submit_blocking(device, "cloud noise bake", [&](VkCommandBuffer cmd) {
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            const Job& j = jobs[i];
            detail::image_barrier(cmd, j.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_PIPELINE_STAGE_2_NONE, 0,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            j.kernel->dispatch(cmd, sets[i], &j.push, j.groups[0], j.groups[1], j.groups[2]);
            detail::image_barrier(cmd, j.image, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    });
    if (!baked) return std::unexpected(baked.error());
    return out;
}

} // namespace engine::renderer
