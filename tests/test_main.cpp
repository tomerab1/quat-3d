// engine_tests entry point (GoogleTest).
//
//   engine_tests                          all tests (GPU tests skip without a device)
//   engine_tests --gtest_filter='-Gpu*'   CPU tests only
//   engine_tests --gtest_filter='Gpu*'    GPU tests only
//   engine_tests --require-gpu            fail GPU tests instead of skipping them
//   engine_tests --validation             GPU tests under the Khronos validation layer
//
// QUAT_REQUIRE_GPU=1 / QUAT_VALIDATION=1 do the same as the flags (CI).

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gtest/gtest.h>

#include "support/gpu_context.hpp"

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv); // strips --gtest_* flags

    quat::test::GpuOptions& options = quat::test::gpu_options();
    options.require_gpu = std::getenv("QUAT_REQUIRE_GPU") != nullptr;
    options.validation = std::getenv("QUAT_VALIDATION") != nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--require-gpu") == 0) {
            options.require_gpu = true;
        } else if (std::strcmp(argv[i], "--validation") == 0) {
            options.validation = true;
        }
    }

    const int result = RUN_ALL_TESTS();
    // Destroying the device reports leaked objects; those messages arrive
    // after the last test, so they fail the run here.
    quat::test::shutdown_gpu_context();
    if (const std::size_t errors = quat::test::validation_errors_outside_tests(); errors > 0) {
        std::fprintf(stderr, "%zu Vulkan validation error(s) outside any test\n", errors);
        return 1;
    }
    return result;
}
