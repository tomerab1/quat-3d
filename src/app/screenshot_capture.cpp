// ScreenshotCapture — see screenshot_capture.hpp.

#include "app/screenshot_capture.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

#include "engine/remote/image_encode.hpp"
#include "engine/core/profile.hpp"

namespace quat::app {

namespace {

namespace remote = engine::remote;
using engine::core::Error;
using remote::Json;

constexpr std::size_t k_max_pending_shots = 8;

// Which 8-bit layouts the readback understands; nullopt for anything else.
[[nodiscard]] std::optional<bool> is_bgra(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: return true;
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: return false;
    default: return std::nullopt;
    }
}

} // namespace

void ScreenshotCapture::request(const remote::ScreenshotRequest& req,
                                remote::Responder responder) {
    std::size_t in_flight = pending.size() + encodings.size();
    for (const Slot& s : slots) in_flight += s.waiting.size();
    if (in_flight >= k_max_pending_shots) {
        responder.reply(remote::rpc_fail(remote::rpc_code::failed,
                                         "too many screenshots in flight; retry shortly"));
        return;
    }
    pending.push_back({req.max_size, std::move(responder), req.camera, next_id++});
}

std::optional<remote::CameraPose> ScreenshotCapture::begin_frame() {
    frame_view = 0;
    for (const PendingShot& p : pending) {
        if (p.view) {
            frame_view = p.id;
            return p.view;
        }
    }
    return std::nullopt;
}

void ScreenshotCapture::finish_encodings() {
    for (auto it = encodings.begin(); it != encodings.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto shot = it->result.get();
        if (shot) {
            it->responder.reply(Json{{"mime", "image/png"},
                                     {"width", shot->width},
                                     {"height", shot->height},
                                     {"data", std::move(shot->base64)}});
        } else {
            it->responder.reply(remote::rpc_fail(remote::rpc_code::failed,
                                                 "screenshot: " + shot.error().message));
        }
        it = encodings.erase(it);
    }
}

void ScreenshotCapture::record(VkCommandBuffer cmd, std::uint32_t slot_index,
                               const CaptureSource& src,
                               engine::rhi::GpuAllocator& allocator) {
    if (pending.empty() || slot_index >= slots.size()) return;
    Slot& slot = slots[slot_index];
    if (!slot.waiting.empty()) return; // on_frame_complete not run yet: next frame
    // The requests this frame was rendered for: the one whose camera it used,
    // or every viewport request when it used the viewport camera.
    std::vector<PendingShot> taken;
    for (auto it = pending.begin(); it != pending.end();) {
        const bool match = frame_view == 0 ? !it->view.has_value() : it->id == frame_view;
        if (match) {
            taken.push_back(std::move(*it));
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
    if (taken.empty()) return;
    const std::optional<bool> bgra = is_bgra(src.format);
    const auto fail_pending = [&](const std::string& why) {
        for (PendingShot& p : taken) {
            p.responder.reply(remote::rpc_fail(remote::rpc_code::failed, "screenshot: " + why));
        }
    };
    if (!bgra || src.image == VK_NULL_HANDLE || src.extent.width == 0 || src.extent.height == 0) {
        fail_pending("the frame's colour format is not 8-bit RGBA/BGRA");
        return;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(src.extent.width) * src.extent.height * 4;
    if (slot.buffer.handle() == VK_NULL_HANDLE || slot.buffer.size() < bytes) {
        slot.buffer = {};
        auto buf = allocator.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_AUTO,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!buf || buf->mapped() == nullptr) {
            fail_pending("could not allocate a readback buffer");
            return;
        }
        slot.buffer = std::move(*buf);
    }

    VkImageMemoryBarrier2 to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = src.layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src.image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {src.extent.width, src.extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           slot.buffer.handle(), 1, &region);

    // Back to the layout the frame left it in (later consumers — present,
    // the next frame's import — are ordered by the submit's semaphore /
    // the slot fence), and make the copy visible to host reads.
    VkImageMemoryBarrier2 back = to_src;
    back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = 0;
    back.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = src.layout;
    VkBufferMemoryBarrier2 to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = slot.buffer.handle();
    to_host.offset = 0;
    to_host.size = bytes;
    dep.pImageMemoryBarriers = &back;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &to_host;
    vkCmdPipelineBarrier2(cmd, &dep);

    slot.extent = src.extent;
    slot.bgra = *bgra;
    slot.waiting = std::move(taken);
}

void ScreenshotCapture::on_frame_complete(std::uint32_t slot_index) {
    if (slot_index >= slots.size() || slots[slot_index].waiting.empty()) return;
    Slot& slot = slots[slot_index];
    std::vector<PendingShot> waiting = std::move(slot.waiting);
    slot.waiting.clear();
    if (auto ok = slot.buffer.invalidate_mapped(); !ok) {
        for (PendingShot& p : waiting) {
            p.responder.reply(remote::rpc_fail(remote::rpc_code::failed,
                                               "screenshot: " + ok.error().message));
        }
        return;
    }
    // Copy out (fast) so the slot's buffer is free for the next frame;
    // resample + PNG + base64 run on a worker thread.
    const std::size_t bytes = static_cast<std::size_t>(slot.extent.width) * slot.extent.height * 4;
    const auto* mapped = static_cast<const std::uint8_t*>(slot.buffer.mapped());
    auto pixels = std::make_shared<const std::vector<std::uint8_t>>(mapped, mapped + bytes);
    const VkExtent2D extent = slot.extent;
    const bool bgra = slot.bgra;
    for (PendingShot& p : waiting) {
        Encoding enc;
        enc.responder = std::move(p.responder);
        enc.result = std::async(std::launch::async, [pixels, extent, bgra,
                                                     max_size = p.max_size]()
                                                        -> std::expected<EncodedShot, Error> {
            QUAT_THREAD_NAME("screenshot encoder");
            QUAT_ZONE_N("encode_screenshot");
            auto img = remote::encode_screenshot(*pixels, extent.width, extent.height,
                                                 static_cast<std::size_t>(extent.width) * 4,
                                                 bgra, max_size);
            if (!img) return std::unexpected(img.error());
            return EncodedShot{img->width, img->height, remote::base64_encode(img->png)};
        });
        encodings.push_back(std::move(enc));
    }
}

} // namespace quat::app
