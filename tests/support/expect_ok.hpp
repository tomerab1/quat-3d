#pragma once

// GoogleTest assertions for std::expected results: on failure they print the
// carried error message instead of a bare "false".
//
//   ASSERT_OK(load_scene(...));           // fatal
//   EXPECT_OK(engine::scene::parse(...)); // non-fatal
//   ASSERT_ERR(parse("{bad"));            // must fail

#include <gtest/gtest.h>

// Variadic so expressions containing commas (template arguments) work.
#define QUAT_EXPECTED_OK_IMPL_(fail_macro, ...)                                           \
    do {                                                                                  \
        const auto quat_result_ = (__VA_ARGS__);                                          \
        if (!quat_result_.has_value()) {                                                  \
            fail_macro() << #__VA_ARGS__ << " failed: " << quat_result_.error().message;  \
        }                                                                                 \
    } while (false)

#define ASSERT_OK(...) QUAT_EXPECTED_OK_IMPL_(FAIL, __VA_ARGS__)
#define EXPECT_OK(...) QUAT_EXPECTED_OK_IMPL_(ADD_FAILURE, __VA_ARGS__)

#define ASSERT_ERR(...)                                                                   \
    do {                                                                                  \
        const auto quat_result_ = (__VA_ARGS__);                                          \
        ASSERT_FALSE(quat_result_.has_value()) << #__VA_ARGS__ << " unexpectedly succeeded"; \
    } while (false)
