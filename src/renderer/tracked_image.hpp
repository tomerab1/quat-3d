#pragma once

// A persistent image the render graph imports every frame (R13.3 / R13.4:
// the aerial perspective and fog volumes, the cloud and fog histories). The
// render graph only creates 2D transients; these images are 3D or must
// outlive a frame, so they are owned here and imported in the state the
// previous frame left them, with that frame's access as the "available"
// scope (the TaaHistory pattern). Renderer-internal.

#include <cstdint>
#include <expected>
#include <utility>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::renderer::detail {

struct TrackedImage {
    rhi::GpuImage         image;
    rhi::ImageView        view;
    VkFormat              format = VK_FORMAT_UNDEFINED;
    VkExtent3D            extent{0, 0, 0};
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = 0;

    [[nodiscard]] bool valid() const { return view.handle() != VK_NULL_HANDLE; }

    // A 2D (depth 1) or 3D storage + sampled image.
    [[nodiscard]] static std::expected<TrackedImage, core::Error>
    create(VkDevice device, rhi::GpuAllocator& allocator, VkFormat format, VkExtent3D extent) {
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const bool volume = extent.depth > 1;
        auto img = volume ? allocator.create_image_3d(format, extent, usage)
                          : allocator.create_image(format, {extent.width, extent.height}, usage);
        if (!img) return std::unexpected(img.error());
        auto v = rhi::create_image_view(device, img->handle(), format, VK_IMAGE_ASPECT_COLOR_BIT,
                                        1, volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
        if (!v) return std::unexpected(v.error());
        TrackedImage out;
        out.image = std::move(*img);
        out.view = std::move(*v);
        out.format = format;
        out.extent = extent;
        return out;
    }

    // Imports the image with its tracked state; the graph hands it back in
    // SHADER_READ_ONLY_OPTIMAL at the end of the frame, which becomes the
    // state the next import starts from. `discard`: the contents are
    // regenerated this frame (imported as UNDEFINED with no wait beyond the
    // frame slot's fence — only for images one frame slot owns).
    [[nodiscard]] rhi::ResourceHandle import(rhi::RenderGraph& graph, const char* name,
                                             bool discard) {
        const rhi::ResourceHandle h = graph.import_image(
            name, image.handle(), view.handle(), format, {extent.width, extent.height},
            discard ? VK_IMAGE_LAYOUT_UNDEFINED : layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            discard ? VK_PIPELINE_STAGE_2_NONE : stage, discard ? 0 : access);
        layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // the final barrier's second scope
        access = 0;
        return h;
    }
};

} // namespace engine::renderer::detail
