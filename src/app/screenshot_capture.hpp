#pragma once

// ScreenshotCapture — the remote `view.screenshot` pipeline behind
// RemoteBridge (ROADMAP A2/A3, app layer).
//
// Screenshots never stall the GPU: record() copies the frame's final scene
// image into a per-frame-slot host-visible buffer, which on_frame_complete()
// reads only once that slot's fence has signalled anyway; resampling, PNG and
// base64 encoding run on a worker thread, and finish_encodings() sends the
// replies from the main thread.
//
// A request may carry its own camera (issue #41). The frame loop asks
// begin_frame() which view the coming frame should render: the oldest such
// request's, else the viewport camera. record() then captures only the
// requests that frame was rendered for; the rest wait for a later frame. The
// viewport camera itself is never modified, but the viewport shows that one
// frame from the requested camera.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/remote/world_commands.hpp" // CameraPose, ScreenshotRequest
#include "engine/rhi/gpu_allocator.hpp"

namespace quat::app {

// The image a screenshot copies: the final scene colour (the editor's
// offscreen viewport, else the swapchain image) as the recorded frame leaves
// it. Needs TRANSFER_SRC usage.
struct CaptureSource {
    VkImage       image = VK_NULL_HANDLE;
    VkFormat      format = VK_FORMAT_UNDEFINED;
    VkExtent2D    extent{0, 0};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // before and after the copy
};

class ScreenshotCapture {
public:
    // `frames_in_flight` sizes the per-slot readback buffers.
    explicit ScreenshotCapture(std::uint32_t frames_in_flight) : slots(frames_in_flight) {}

    // Queues a request for the next recorded frame (rendered from its
    // camera, if it has one); replies "too many screenshots in flight" right
    // away past a small limit.
    void request(const engine::remote::ScreenshotRequest& request,
                 engine::remote::Responder responder);
    // Before the frame's view is computed: the camera a pending request needs
    // this frame rendered from, or nullopt for the viewport camera.
    [[nodiscard]] std::optional<engine::remote::CameraPose> begin_frame();
    // Replies for encodings that have finished (main thread, once per frame).
    void finish_encodings();
    // After the frame's graph is recorded: copies `src` for pending requests.
    void record(VkCommandBuffer cmd, std::uint32_t slot_index, const CaptureSource& src,
                engine::rhi::GpuAllocator& allocator);
    // Right after the slot's fence wait: hands its readback to an encoder.
    void on_frame_complete(std::uint32_t slot_index);

private:
    struct EncodedShot {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::string   base64;
    };
    struct PendingShot {
        std::uint32_t                             max_size = 1024;
        engine::remote::Responder                 responder;
        std::optional<engine::remote::CameraPose> view; // its own camera
        std::uint64_t                             id = 0;
    };
    // One readback buffer per frame-in-flight slot; `waiting` are the
    // requests whose copy was recorded into that slot's last submission.
    struct Slot {
        engine::rhi::GpuBuffer   buffer;
        VkExtent2D               extent{0, 0};
        bool                     bgra = false;
        std::vector<PendingShot> waiting;
    };
    struct Encoding {
        std::future<std::expected<EncodedShot, engine::core::Error>> result;
        engine::remote::Responder                                    responder;
    };

    std::vector<PendingShot> pending;
    std::uint64_t            next_id = 1;
    std::uint64_t            frame_view = 0; // id of the request this frame renders for; 0 = viewport
    std::vector<Slot>        slots;
    std::vector<Encoding>    encodings;
};

} // namespace quat::app
