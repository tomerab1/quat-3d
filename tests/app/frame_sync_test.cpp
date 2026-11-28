// Issue #110: the frame loop's fences never leave it blocked. A fake queue
// (QueueSyncOps) stands in for the GPU: it tracks each fence's state and
// records a wait on a fence nothing will ever signal — the hang the issue
// describes — instead of blocking the test.

#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "app/frame_sync.hpp"

namespace {

using quat::app::FrameFences;
using quat::app::GpuFault;
using quat::app::GpuFaultKind;
using quat::app::QueueSyncOps;

struct FakeGpu {
    struct Fence {
        bool signaled = false;
        bool pending = false; // a queued submit will signal it
        bool alive = true;
    };
    std::vector<Fence> fences;

    VkResult submit_result = VK_SUCCESS;       // submits with work
    VkResult empty_submit_result = VK_SUCCESS; // fence-only submits
    VkResult wait_result = VK_SUCCESS;         // forced result of the next waits
    bool     hung = false;                     // queued work never finishes

    int           waits = 0;
    bool          waited_forever = false; // a wait no submit would ever end
    std::uint64_t last_timeout = 0;
};

// Fake handles are fence indices + 1 (a non-dispatchable handle is 64 bits:
// a pointer or a uint64_t, depending on the platform).
[[nodiscard]] VkFence to_handle(std::size_t index) {
    return std::bit_cast<VkFence>(static_cast<std::uint64_t>(index + 1));
}

[[nodiscard]] std::size_t to_index(VkFence fence) {
    return static_cast<std::size_t>(std::bit_cast<std::uint64_t>(fence) - 1);
}

class FakeOps final : public QueueSyncOps {
public:
    explicit FakeOps(FakeGpu& gpu) : gpu_(gpu) {}

    VkResult create_fence(bool signaled, VkFence* out) override {
        gpu_.fences.push_back({.signaled = signaled});
        *out = to_handle(gpu_.fences.size() - 1);
        return VK_SUCCESS;
    }
    void destroy_fence(VkFence fence) override { gpu_.fences[to_index(fence)].alive = false; }
    VkResult wait_fence(VkFence fence, std::uint64_t timeout_ns) override {
        ++gpu_.waits;
        gpu_.last_timeout = timeout_ns;
        if (gpu_.wait_result != VK_SUCCESS) return gpu_.wait_result;
        FakeGpu::Fence& f = gpu_.fences[to_index(fence)];
        EXPECT_TRUE(f.alive) << "waited on a destroyed fence";
        if (f.signaled) return VK_SUCCESS;
        if (f.pending && !gpu_.hung) {
            f.signaled = true; // the GPU finished the work
            f.pending = false;
            return VK_SUCCESS;
        }
        if (!f.pending) gpu_.waited_forever = true; // nothing will ever signal it
        return VK_TIMEOUT;
    }
    VkResult reset_fence(VkFence fence) override {
        gpu_.fences[to_index(fence)].signaled = false;
        return VK_SUCCESS;
    }
    VkResult submit(std::span<const VkSubmitInfo2> submits, VkFence fence) override {
        const VkResult r = submits.empty() ? gpu_.empty_submit_result : gpu_.submit_result;
        if (r == VK_SUCCESS) gpu_.fences[to_index(fence)].pending = true;
        return r;
    }

private:
    FakeGpu& gpu_;
};

constexpr std::uint64_t k_timeout = 2'000'000'000ULL;

[[nodiscard]] VkSubmitInfo2 frame_submit() {
    VkSubmitInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    return info;
}

// One slot through wait -> reset -> submit, as FrameRenderer runs it.
void begin_frame(FrameFences& fences, std::uint32_t slot) {
    ASSERT_TRUE(fences.wait(slot).has_value());
    ASSERT_TRUE(fences.reset(slot).has_value());
}

TEST(FrameFences, NormalFramesWaitWithTheFiniteTimeout) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    for (std::uint32_t frame = 0; frame < 6; ++frame) {
        const std::uint32_t slot = frame % 2;
        begin_frame(fences, slot);
        ASSERT_TRUE(fences.submit(slot, frame_submit()).has_value());
    }
    EXPECT_EQ(gpu.last_timeout, k_timeout) << "never UINT64_MAX";
    EXPECT_FALSE(gpu.waited_forever);
    EXPECT_FALSE(fences.fault().has_value());
}

TEST(FrameFences, WaitForSlotReturnsAfterAFailedSubmit) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    begin_frame(fences, 0);
    gpu.submit_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto submitted = fences.submit(0, frame_submit());
    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().kind, GpuFaultKind::submit_failed);
    EXPECT_EQ(submitted.error().result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_EQ(submitted.error().call, "vkQueueSubmit2");

    // The slot comes round again: the wait returns, it does not hang on the
    // fence the failed frame reset.
    EXPECT_TRUE(fences.wait(0).has_value());
    EXPECT_FALSE(gpu.waited_forever);
    ASSERT_TRUE(fences.fault().has_value());
    EXPECT_EQ(fences.fault()->kind, GpuFaultKind::submit_failed);
}

TEST(FrameFences, FenceIsRecreatedSignalledWhenTheQueueRefusesAnEmptySubmit) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    begin_frame(fences, 1);
    const VkFence before = fences.fence(1);
    gpu.submit_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    gpu.empty_submit_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    ASSERT_FALSE(fences.submit(1, frame_submit()).has_value());

    EXPECT_NE(fences.fence(1), before);
    EXPECT_FALSE(gpu.fences[to_index(before)].alive) << "the unsignalled fence is destroyed";
    EXPECT_TRUE(fences.wait(1).has_value());
    EXPECT_FALSE(gpu.waited_forever);
}

TEST(FrameFences, DeviceLostOnSubmitIsReportedAndNeverWaitedOn) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    begin_frame(fences, 0);
    gpu.submit_result = VK_ERROR_DEVICE_LOST;
    gpu.empty_submit_result = VK_ERROR_DEVICE_LOST;
    const auto submitted = fences.submit(0, frame_submit());
    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().kind, GpuFaultKind::device_lost);

    const int waits = gpu.waits;
    for (const std::uint32_t slot : {0U, 1U}) {
        const auto waited = fences.wait(slot);
        ASSERT_FALSE(waited.has_value());
        EXPECT_EQ(waited.error().kind, GpuFaultKind::device_lost);
    }
    EXPECT_EQ(gpu.waits, waits) << "the lost device is not waited on";
    EXPECT_FALSE(gpu.waited_forever);
}

TEST(FrameFences, DeviceLostOnWaitIsReported) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    gpu.wait_result = VK_ERROR_DEVICE_LOST;
    const auto waited = fences.wait(0);
    ASSERT_FALSE(waited.has_value());
    EXPECT_EQ(waited.error().kind, GpuFaultKind::device_lost);
    EXPECT_EQ(waited.error().call, "vkWaitForFences");
    EXPECT_FALSE(fences.reset(0).has_value()) << "no reset after a device loss";
}

TEST(FrameFences, AFrameThatNeverFinishesIsAHang) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    begin_frame(fences, 0);
    ASSERT_TRUE(fences.submit(0, frame_submit()).has_value());
    gpu.hung = true; // e.g. a material shader looping forever
    const auto waited = fences.wait(0);
    ASSERT_FALSE(waited.has_value());
    EXPECT_EQ(waited.error().kind, GpuFaultKind::hang);
    EXPECT_EQ(gpu.last_timeout, k_timeout);
    const int waits = gpu.waits;
    EXPECT_FALSE(fences.wait(1).has_value()) << "a hung GPU is not waited on again";
    EXPECT_EQ(gpu.waits, waits);
}

TEST(FrameFences, TheFirstFaultIsKept) {
    FakeGpu gpu;
    FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<FakeOps>(gpu), 2, k_timeout).has_value());
    (void)fences.report(quat::app::classify_gpu_fault(VK_ERROR_DEVICE_LOST, "vkQueuePresentKHR"));
    (void)fences.report(
        quat::app::classify_gpu_fault(VK_ERROR_OUT_OF_DATE_KHR, "vkAcquireNextImageKHR"));
    ASSERT_TRUE(fences.fault().has_value());
    EXPECT_EQ(fences.fault()->kind, GpuFaultKind::device_lost);
    EXPECT_EQ(fences.fault()->call, "vkQueuePresentKHR");
}

TEST(GpuFault, ClassifiesAndDescribesResults) {
    using quat::app::classify_gpu_fault;
    EXPECT_EQ(classify_gpu_fault(VK_ERROR_DEVICE_LOST, "x").kind, GpuFaultKind::device_lost);
    EXPECT_EQ(classify_gpu_fault(VK_TIMEOUT, "x").kind, GpuFaultKind::hang);
    EXPECT_EQ(classify_gpu_fault(VK_ERROR_OUT_OF_HOST_MEMORY, "x").kind,
              GpuFaultKind::sync_failed);
    EXPECT_EQ(
        classify_gpu_fault(VK_ERROR_OUT_OF_HOST_MEMORY, "x", GpuFaultKind::submit_failed).kind,
        GpuFaultKind::submit_failed);
    const std::string line = quat::app::describe_gpu_fault(
        GpuFault{GpuFaultKind::device_lost, VK_ERROR_DEVICE_LOST, "vkQueueSubmit2"});
    EXPECT_NE(line.find("GPU device lost"), std::string::npos) << line;
    EXPECT_NE(line.find("vkQueueSubmit2"), std::string::npos) << line;
    EXPECT_NE(line.find("-4"), std::string::npos) << line;
}

} // namespace
