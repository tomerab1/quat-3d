#pragma once

// GoogleTest assertions for std::expected results: on failure they print the
// carried error message instead of a bare "false".
//
//   ASSERT_OK(load_scene(...));           // fatal
//   EXPECT_OK(engine::scene::parse(...)); // non-fatal
//   ASSERT_ERR(parse("{bad"));            // must fail

#include <string>

#include <gtest/gtest.h>

// Variadic so expressions containing commas (template arguments) work; the
// if/else form (as GoogleTest's own macros use) lets callers stream extra
// context: ASSERT_OK(apply(x)) << "while applying " << name;
#define QUAT_EXPECTED_IMPL_(want_ok, fail_macro, ...)                                     \
    switch (0)                                                                            \
    case 0:                                                                               \
    default:                                                                              \
        if (const auto& quat_result_ = (__VA_ARGS__); quat_result_.has_value() == (want_ok)) { \
        } else                                                                            \
            fail_macro() << #__VA_ARGS__                                                  \
                         << ((want_ok) ? " failed: " : " unexpectedly succeeded")         \
                         << ((want_ok) ? ::quat::test::error_text(quat_result_) : "") << ' '

namespace quat::test {
template <typename Expected>
[[nodiscard]] std::string error_text(const Expected& e) {
    return e.has_value() ? std::string() : e.error().message;
}
} // namespace quat::test

#define ASSERT_OK(...) QUAT_EXPECTED_IMPL_(true, FAIL, __VA_ARGS__)
#define EXPECT_OK(...) QUAT_EXPECTED_IMPL_(true, ADD_FAILURE, __VA_ARGS__)
#define ASSERT_ERR(...) QUAT_EXPECTED_IMPL_(false, FAIL, __VA_ARGS__)
#define EXPECT_ERR(...) QUAT_EXPECTED_IMPL_(false, ADD_FAILURE, __VA_ARGS__)
