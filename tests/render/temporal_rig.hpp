#pragma once

// Rig for temporal resolve / upscaler tests (R11): per frame, render-extent
// inputs (linear HDR colour, depth, velocity) are uploaded from the host, one
// resolve runs through a render graph, and the display-extent output is read
// back. The resolve is any callable (graph, UpscalerInputs) ->
// expected<ResourceHandle>, so TaaPass and the Upscaler interface share it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <vulkan/vulkan.h>

#include "engine/renderer/upscaler.hpp"
#include "engine/renderer/velocity.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace quat::test {

// One frame's render-extent inputs, row-major. Empty velocity = zero, empty
// depth = 0.5 everywhere.
struct TemporalFrame {
    std::vector<glm::vec4>         color;
    std::vector<glm::vec2>         velocity;
    std::vector<float>             depth;
    engine::renderer::ViewJitter   jitter{};
    float                          sharpness = 0.0F;
    bool                           reset = false;
};

class TemporalRig {
public:
    [[nodiscard]] static std::expected<TemporalRig, engine::core::Error>
    create(GpuContext& g, VkExtent2D render, VkExtent2D display) {
        TemporalRig rig;
        rig.g_ = &g;
        rig.render_ = render;
        rig.display_ = display;
        constexpr VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkDevice dev = g.device.handle();
        auto color = g.allocator.create_image(VK_FORMAT_R16G16B16A16_SFLOAT, render, usage);
        auto velocity = g.allocator.create_image(engine::renderer::velocity_format, render, usage);
        auto depth = g.allocator.create_image(VK_FORMAT_D32_SFLOAT, render, usage);
        if (!color || !velocity || !depth) return fail("temporal rig: image creation failed");
        auto color_view = engine::rhi::create_image_view(dev, color->handle(),
                                                         VK_FORMAT_R16G16B16A16_SFLOAT);
        auto velocity_view = engine::rhi::create_image_view(dev, velocity->handle(),
                                                            engine::renderer::velocity_format);
        auto depth_view = engine::rhi::create_image_view(dev, depth->handle(),
                                                         VK_FORMAT_D32_SFLOAT,
                                                         VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!color_view || !velocity_view || !depth_view) {
            return fail("temporal rig: view creation failed");
        }
        const VkDeviceSize pixels = static_cast<VkDeviceSize>(render.width) * render.height;
        auto staging = g.allocator.create_buffer(
            pixels * (8 + 4 + 4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!staging) return fail("temporal rig: staging buffer creation failed");
        rig.color_ = std::move(*color);
        rig.velocity_ = std::move(*velocity);
        rig.depth_ = std::move(*depth);
        rig.color_view_ = std::move(*color_view);
        rig.velocity_view_ = std::move(*velocity_view);
        rig.depth_view_ = std::move(*depth_view);
        rig.staging_ = std::move(*staging);
        rig.pool_ = std::make_unique<engine::rhi::TransientImagePool>(g.device, g.allocator);
        return rig;
    }

    [[nodiscard]] VkExtent2D render() const { return render_; }
    [[nodiscard]] VkExtent2D display() const { return display_; }
    [[nodiscard]] std::size_t render_pixels() const {
        return static_cast<std::size_t>(render_.width) * render_.height;
    }

    // Uploads `f`, resolves it with `resolve` and returns the RGBA16F output.
    template <typename Resolve>
    [[nodiscard]] std::expected<HostImage, engine::core::Error>
    frame(const TemporalFrame& f, Resolve&& resolve) {
        const std::size_t n = render_pixels();
        if (f.color.size() != n || (!f.velocity.empty() && f.velocity.size() != n) ||
            (!f.depth.empty() && f.depth.size() != n)) {
            return fail("temporal rig: input sizes do not match the render extent");
        }
        auto* bytes = static_cast<std::uint8_t*>(staging_.mapped());
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec4& c = f.color[i];
            const std::uint16_t h[4] = {glm::packHalf1x16(c.r), glm::packHalf1x16(c.g),
                                        glm::packHalf1x16(c.b), glm::packHalf1x16(c.a)};
            std::memcpy(bytes + i * 8, h, sizeof(h));
            const glm::vec2 v = f.velocity.empty() ? glm::vec2(0.0F) : f.velocity[i];
            const std::uint16_t hv[2] = {glm::packHalf1x16(v.x), glm::packHalf1x16(v.y)};
            std::memcpy(bytes + n * 8 + i * 4, hv, sizeof(hv));
            const float d = f.depth.empty() ? 0.5F : f.depth[i];
            std::memcpy(bytes + n * 12 + i * 4, &d, sizeof(d));
        }
        if (auto fl = staging_.flush(); !fl) return std::unexpected(fl.error());

        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        engine::rhi::RenderGraph graph(*pool_);
        engine::renderer::UpscalerInputs in;
        in.color = graph.import_image("rig_color", color_.handle(), color_view_.handle(),
                                      VK_FORMAT_R16G16B16A16_SFLOAT, render_, ro, ro);
        in.velocity =
            graph.import_image("rig_velocity", velocity_.handle(), velocity_view_.handle(),
                               engine::renderer::velocity_format, render_, ro, ro);
        in.depth = graph.import_image("rig_depth", depth_.handle(), depth_view_.handle(),
                                      VK_FORMAT_D32_SFLOAT, render_, ro, ro);
        in.render_extent = render_;
        in.display_extent = display_;
        in.jitter = f.jitter;
        in.sharpness = f.sharpness;
        in.reset = f.reset;
        auto out = std::forward<Resolve>(resolve)(graph, in);
        if (!out) return std::unexpected(out.error());

        const VkBuffer staging = staging_.handle();
        const VkExtent2D e = render_;
        const std::array<std::pair<VkImage, VkImageAspectFlags>, 3> images{{
            {color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT},
            {velocity_.handle(), VK_IMAGE_ASPECT_COLOR_BIT},
            {depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT},
        }};
        const std::array<VkDeviceSize, 3> offsets{0, n * 8, n * 12};
        auto result = render_to_host(*g_, graph, *out, display_, 8, [&](VkCommandBuffer cmd) {
            for (std::size_t k = 0; k < images.size(); ++k) {
                const auto [image, aspect] = images[k];
                image_barrier(cmd, image, aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy region{};
                region.bufferOffset = offsets[k];
                region.imageSubresource = {aspect, 0, 0, 1};
                region.imageExtent = {e.width, e.height, 1};
                vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &region);
                image_barrier(cmd, image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ro,
                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        });
        pool_->reset(); // the frame finished: transient images are free again
        return result;
    }

private:
    [[nodiscard]] static std::unexpected<engine::core::Error> fail(std::string what) {
        return std::unexpected(engine::core::Error{std::move(what)});
    }

    GpuContext*                                       g_ = nullptr;
    VkExtent2D                                        render_{};
    VkExtent2D                                        display_{};
    engine::rhi::GpuImage                             color_;
    engine::rhi::GpuImage                             velocity_;
    engine::rhi::GpuImage                             depth_;
    engine::rhi::ImageView                            color_view_;
    engine::rhi::ImageView                            velocity_view_;
    engine::rhi::ImageView                            depth_view_;
    engine::rhi::GpuBuffer                            staging_;
    std::unique_ptr<engine::rhi::TransientImagePool>  pool_;
};

// Grey of a pixel (red channel) of an RGBA16F host image.
[[nodiscard]] inline float grey(const HostImage& img, std::uint32_t x, std::uint32_t y) {
    return img.rgba16f(x, y).r;
}

} // namespace quat::test
