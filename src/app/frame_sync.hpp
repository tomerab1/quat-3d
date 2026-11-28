#pragma once

// FrameFences — the frame loop's in-flight fences and the queue submit that
// signals them (issue #110), behind a small seam (QueueSyncOps) so tests can
// make a submit or a wait fail.
//
// The loop never blocks forever on the GPU:
//   * waits use a finite timeout; expiry is reported as a GPU hang;
//   * a failed submit never leaves its slot's fence reset and unsignalled
//     (nothing would ever signal it, and the next wait on that slot would
//     block): the fence is re-signalled with an empty submit, or recreated
//     signalled when that fails too, and the error is reported;
//   * VK_ERROR_DEVICE_LOST from any call is reported as such, and later
//     waits return it at once instead of touching the lost device.
// The first fault is kept (fault()); the frame loop stops on it and takes the
// device-loss path (device_loss.hpp).

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace quat::app {

enum class GpuFaultKind : std::uint8_t {
    device_lost,   // VK_ERROR_DEVICE_LOST
    hang,          // a fence wait ran past the timeout
    submit_failed, // vkQueueSubmit2 failed (out of memory, ...)
    sync_failed,   // a wait / reset / acquire / present failed otherwise
};

[[nodiscard]] const char* gpu_fault_kind_name(GpuFaultKind kind);

struct GpuFault {
    GpuFaultKind kind = GpuFaultKind::sync_failed;
    VkResult     result = VK_SUCCESS;
    std::string  call; // the Vulkan call that reported it, e.g. "vkQueueSubmit2"
};

// The fault a failed `call` reports: device_lost for VK_ERROR_DEVICE_LOST,
// hang for VK_TIMEOUT, otherwise `otherwise`.
[[nodiscard]] GpuFault classify_gpu_fault(VkResult result, std::string call,
                                          GpuFaultKind otherwise = GpuFaultKind::sync_failed);

// One line for the log: kind, call and VkResult.
[[nodiscard]] std::string describe_gpu_fault(const GpuFault& fault);

// The Vulkan calls FrameFences makes, overridable by tests (fault injection).
class QueueSyncOps {
public:
    QueueSyncOps() = default;
    QueueSyncOps(const QueueSyncOps&) = delete;
    QueueSyncOps& operator=(const QueueSyncOps&) = delete;
    virtual ~QueueSyncOps() = default;

    [[nodiscard]] virtual VkResult create_fence(bool signaled, VkFence* out) = 0;
    virtual void                   destroy_fence(VkFence fence) = 0;
    [[nodiscard]] virtual VkResult wait_fence(VkFence fence, std::uint64_t timeout_ns) = 0;
    [[nodiscard]] virtual VkResult reset_fence(VkFence fence) = 0;
    // vkQueueSubmit2 on the frame queue; `submits` may be empty (a fence-only
    // submit, signalled once all earlier work on the queue completes).
    [[nodiscard]] virtual VkResult submit(std::span<const VkSubmitInfo2> submits,
                                          VkFence fence) = 0;
};

// The real calls on `device` / `queue`.
[[nodiscard]] std::unique_ptr<QueueSyncOps> make_vulkan_queue_sync_ops(VkDevice device,
                                                                       VkQueue queue);

class FrameFences {
public:
    // A frame that is still running after this long is a GPU hang. Far
    // above any real frame (a TDR resets the GPU after about 2 s on Windows),
    // and above a heavy frame on a software rasteriser (lavapipe in CI).
    static constexpr std::uint64_t k_default_timeout_ns = 10'000'000'000ULL;

    FrameFences() = default;
    ~FrameFences();
    FrameFences(const FrameFences&) = delete;
    FrameFences& operator=(const FrameFences&) = delete;
    FrameFences(FrameFences&&) = delete;
    FrameFences& operator=(FrameFences&&) = delete;

    // `slots` fences, created signalled. Waits time out after `timeout_ns`.
    [[nodiscard]] std::expected<void, GpuFault> create(std::unique_ptr<QueueSyncOps> ops,
                                                       std::uint32_t slots,
                                                       std::uint64_t timeout_ns);
    // Destroys the fences (the caller has idled the device). Idempotent.
    void destroy();

    // Waits for `slot`'s last submission to finish.
    [[nodiscard]] std::expected<void, GpuFault> wait(std::uint32_t slot);
    // Unsignals `slot`'s fence before recording into it.
    [[nodiscard]] std::expected<void, GpuFault> reset(std::uint32_t slot);
    // Submits `info` signalling `slot`'s fence. On failure the fence is left
    // signalled (see the header comment) and the fault returned.
    [[nodiscard]] std::expected<void, GpuFault> submit(std::uint32_t slot,
                                                       const VkSubmitInfo2& info);

    // Records a fault found outside these calls (acquire, present) so it is
    // reported like the others. Returns it.
    std::unexpected<GpuFault> report(GpuFault fault);

    // The first fault, if any.
    [[nodiscard]] const std::optional<GpuFault>& fault() const { return fault_; }
    [[nodiscard]] VkFence fence(std::uint32_t slot) const { return fences_[slot]; }
    [[nodiscard]] std::uint64_t timeout_ns() const { return timeout_ns_; }

private:
    // After a device loss or a hang the GPU is not waited on again.
    [[nodiscard]] bool gpu_unusable() const;
    // Leaves `slot`'s fence signalled after a failed submit.
    void resignal(std::uint32_t slot);

    std::unique_ptr<QueueSyncOps> ops_;
    std::vector<VkFence>          fences_;
    std::uint64_t                 timeout_ns_ = k_default_timeout_ns;
    std::optional<GpuFault>       fault_;
};

} // namespace quat::app
