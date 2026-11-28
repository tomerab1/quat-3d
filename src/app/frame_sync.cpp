#include "app/frame_sync.hpp"

#include <cstdio>
#include <utility>

namespace quat::app {
namespace {

class VulkanQueueSyncOps final : public QueueSyncOps {
public:
    VulkanQueueSyncOps(VkDevice device, VkQueue queue) : device_(device), queue_(queue) {}

    VkResult create_fence(bool signaled, VkFence* out) override {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0U;
        return vkCreateFence(device_, &info, nullptr, out);
    }
    void destroy_fence(VkFence fence) override { vkDestroyFence(device_, fence, nullptr); }
    VkResult wait_fence(VkFence fence, std::uint64_t timeout_ns) override {
        return vkWaitForFences(device_, 1, &fence, VK_TRUE, timeout_ns);
    }
    VkResult reset_fence(VkFence fence) override { return vkResetFences(device_, 1, &fence); }
    VkResult submit(std::span<const VkSubmitInfo2> submits, VkFence fence) override {
        return vkQueueSubmit2(queue_, static_cast<std::uint32_t>(submits.size()),
                              submits.empty() ? nullptr : submits.data(), fence);
    }

private:
    VkDevice device_;
    VkQueue  queue_;
};

} // namespace

const char* gpu_fault_kind_name(GpuFaultKind kind) {
    switch (kind) {
    case GpuFaultKind::device_lost: return "GPU device lost";
    case GpuFaultKind::hang: return "GPU hang (fence wait timed out)";
    case GpuFaultKind::submit_failed: return "GPU submit failed";
    case GpuFaultKind::sync_failed: return "GPU synchronisation failed";
    }
    return "GPU fault";
}

GpuFault classify_gpu_fault(VkResult result, std::string call, GpuFaultKind otherwise) {
    GpuFaultKind kind = otherwise;
    if (result == VK_ERROR_DEVICE_LOST) {
        kind = GpuFaultKind::device_lost;
    } else if (result == VK_TIMEOUT) {
        kind = GpuFaultKind::hang;
    }
    return GpuFault{kind, result, std::move(call)};
}

std::string describe_gpu_fault(const GpuFault& fault) {
    return std::string(gpu_fault_kind_name(fault.kind)) + ": " + fault.call + " returned " +
           std::to_string(static_cast<int>(fault.result));
}

std::unique_ptr<QueueSyncOps> make_vulkan_queue_sync_ops(VkDevice device, VkQueue queue) {
    return std::make_unique<VulkanQueueSyncOps>(device, queue);
}

FrameFences::~FrameFences() {
    destroy();
}

std::expected<void, GpuFault> FrameFences::create(std::unique_ptr<QueueSyncOps> ops,
                                                  std::uint32_t slots,
                                                  std::uint64_t timeout_ns) {
    destroy();
    ops_ = std::move(ops);
    timeout_ns_ = timeout_ns;
    fault_.reset();
    fences_.assign(slots, VK_NULL_HANDLE);
    for (VkFence& f : fences_) {
        if (const VkResult r = ops_->create_fence(true, &f); r != VK_SUCCESS) {
            f = VK_NULL_HANDLE;
            return std::unexpected(classify_gpu_fault(r, "vkCreateFence"));
        }
    }
    return {};
}

void FrameFences::destroy() {
    if (ops_) {
        for (VkFence f : fences_) {
            if (f != VK_NULL_HANDLE) ops_->destroy_fence(f);
        }
    }
    fences_.clear();
}

bool FrameFences::gpu_unusable() const {
    return fault_ &&
           (fault_->kind == GpuFaultKind::device_lost || fault_->kind == GpuFaultKind::hang);
}

std::unexpected<GpuFault> FrameFences::report(GpuFault fault) {
    if (!fault_) fault_ = fault;
    return std::unexpected(std::move(fault));
}

std::expected<void, GpuFault> FrameFences::wait(std::uint32_t slot) {
    if (gpu_unusable()) return std::unexpected(*fault_);
    const VkResult r = ops_->wait_fence(fences_[slot], timeout_ns_);
    if (r == VK_SUCCESS) return {};
    return report(classify_gpu_fault(r, "vkWaitForFences"));
}

std::expected<void, GpuFault> FrameFences::reset(std::uint32_t slot) {
    if (gpu_unusable()) return std::unexpected(*fault_);
    const VkResult r = ops_->reset_fence(fences_[slot]);
    if (r == VK_SUCCESS) return {};
    return report(classify_gpu_fault(r, "vkResetFences"));
}

std::expected<void, GpuFault> FrameFences::submit(std::uint32_t slot, const VkSubmitInfo2& info) {
    const VkResult r = ops_->submit(std::span<const VkSubmitInfo2>(&info, 1), fences_[slot]);
    if (r == VK_SUCCESS) return {};
    // The batch was not queued, so nothing will signal the fence the caller
    // reset for it: leave it signalled before reporting.
    resignal(slot);
    return report(classify_gpu_fault(r, "vkQueueSubmit2", GpuFaultKind::submit_failed));
}

void FrameFences::resignal(std::uint32_t slot) {
    // A fence-only submit signals once the queue's earlier work is done.
    if (ops_->submit({}, fences_[slot]) == VK_SUCCESS) return;
    // The queue refuses work too (device lost, out of memory): replace the
    // fence with one created signalled.
    VkFence fresh = VK_NULL_HANDLE;
    if (const VkResult r = ops_->create_fence(true, &fresh); r != VK_SUCCESS) {
        // Nothing left to signal with. The fault is reported by the caller,
        // and the loop stops on it, so this fence is never waited on again.
        std::fprintf(stderr, "[error] could not re-create frame fence %u (%d)\n", slot,
                     static_cast<int>(r));
        return;
    }
    ops_->destroy_fence(fences_[slot]);
    fences_[slot] = fresh;
}

} // namespace quat::app
