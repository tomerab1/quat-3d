// Whole-image byte copies between render-graph images (see add_byte_copy in
// render_graph.hpp for why both sides must share a storage format).

#include <format>

#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {

std::expected<void, core::Error> add_byte_copy(RenderGraph& graph, const char* name,
                                               ResourceHandle src, ResourceHandle dst,
                                               VkExtent2D extent) {
    const VkFormat src_format = graph.storage_format(src);
    const VkFormat dst_format = graph.storage_format(dst);
    if (src_format == VK_FORMAT_UNDEFINED || dst_format == VK_FORMAT_UNDEFINED) {
        return std::unexpected(
            core::Error{std::format("byte copy '{}': source or destination is not an image", name)});
    }
    if (src_format != dst_format) {
        return std::unexpected(core::Error{std::format(
            "byte copy '{}': the source is stored as format {} and the destination as {}; store "
            "both in one format (create_transient_image's storage_format) instead of "
            "reinterpreting the bytes in the copy",
            name, static_cast<int>(src_format), static_cast<int>(dst_format))});
    }
    (void)graph.add_pass(name, PassType::transfer)
        .reads(src, ResourceUsage::transfer_src)
        .writes(dst, ResourceUsage::transfer_dst)
        .execute([src, dst, extent](PassContext& ctx) {
            VkImageCopy2 region{};
            region.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {extent.width, extent.height, 1};
            VkCopyImageInfo2 copy{};
            copy.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
            copy.srcImage = ctx.resolve(src).image;
            copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            copy.dstImage = ctx.resolve(dst).image;
            copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copy.regionCount = 1;
            copy.pRegions = &region;
            vkCmdCopyImage2(ctx.cmd(), &copy);
        });
    return {};
}

} // namespace engine::rhi
