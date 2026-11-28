#pragma once

// Shared GPU context for tests: one surface-less Vulkan device (a real GPU,
// or Mesa's lavapipe on CI) created on first use and torn down when the test
// program exits.
//
// GPU test suites are named Gpu* so CTest can run them separately, and start
// with QUAT_REQUIRE_GPU(ctx): it skips the test when no Vulkan device exists,
// or fails it when the run demands a GPU (--require-gpu or
// QUAT_REQUIRE_GPU=1).
//
// With --validation (QUAT_VALIDATION=1) the device runs under the Khronos
// validation layer, and every error-severity message fails the running test
// (the message names it); one outside any test fails the program. A run that
// asks for validation without the layer installed gets no device.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace engine::rhi {
enum class DebugMessageSeverity : std::uint8_t;
class Device;
class GpuAllocator;
class PipelineCache;
struct TransferContext;
} // namespace engine::rhi

namespace quat::test {

struct GpuContext {
    engine::rhi::Device&                device;
    engine::rhi::GpuAllocator&          allocator;
    engine::rhi::PipelineCache&         cache;
    const engine::rhi::TransferContext& transfer;
    std::string                         shader_dir; // cooked SPIR-V + reflection
};

// Options set by the test main from the command line / environment.
struct GpuOptions {
    bool require_gpu = false;
    bool validation = false;
};
GpuOptions& gpu_options();

// The shared context, or null with `why` filled when no device is available.
[[nodiscard]] GpuContext* gpu_context(std::string& why);

// Destroys the context (called once at exit by the test main).
void shutdown_gpu_context();

// Device::CreateInfo::on_debug_message for test devices: an error-severity
// message fails the running test via ADD_FAILURE, naming the test; outside
// any test it is counted (validation_errors_outside_tests). Tests that create
// their own validated Device pass it too.
void report_validation_message(engine::rhi::DebugMessageSeverity severity,
                               std::string_view message);

// Error-severity validation messages reported while no test was running
// (e.g. object leaks found at device destruction). The test main fails the
// run when this is non-zero after shutdown_gpu_context().
[[nodiscard]] std::size_t validation_errors_outside_tests();

} // namespace quat::test

#define QUAT_REQUIRE_GPU(ctx_var)                                                         \
    std::string quat_gpu_why_;                                                            \
    ::quat::test::GpuContext* ctx_var = ::quat::test::gpu_context(quat_gpu_why_);         \
    if (ctx_var == nullptr) {                                                             \
        if (::quat::test::gpu_options().require_gpu) {                                    \
            FAIL() << "no Vulkan device: " << quat_gpu_why_;                              \
        }                                                                                 \
        GTEST_SKIP() << "no Vulkan device: " << quat_gpu_why_;                            \
    }
