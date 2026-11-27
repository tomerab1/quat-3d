#pragma once

// Shared GPU context for tests: one surface-less Vulkan device (a real GPU,
// or Mesa's lavapipe on CI) created on first use and torn down when the test
// program exits.
//
// GPU test suites are named Gpu* so CTest can run them separately, and start
// with QUAT_REQUIRE_GPU(ctx): it skips the test when no Vulkan device exists,
// or fails it when the run demands a GPU (--require-gpu or
// QUAT_REQUIRE_GPU=1).

#include <string>

#include <gtest/gtest.h>

namespace engine::rhi {
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
