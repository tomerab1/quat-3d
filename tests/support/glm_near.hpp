#pragma once

// GoogleTest assertions for glm vectors and rotations: on failure they print
// both values and their distance instead of a bare "false".
//
//   EXPECT_VEC_NEAR(world[1][3], glm::vec4(2, 0, 0, 1), 1e-5F);
//   EXPECT_SAME_ROTATION(q, glm::quat(1, 0, 0, 0), 1e-4F); // q and -q are equal

#include <sstream>
#include <string>
#include <type_traits>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

namespace quat::test {

template <glm::length_t L, typename T, glm::qualifier Q>
[[nodiscard]] std::string glm_text(const glm::vec<L, T, Q>& v) {
    std::ostringstream out;
    out << '(';
    for (glm::length_t i = 0; i < L; ++i) out << (i > 0 ? ", " : "") << v[i];
    out << ')';
    return out.str();
}

template <typename T, glm::qualifier Q>
[[nodiscard]] std::string glm_text(const glm::qua<T, Q>& q) {
    std::ostringstream out;
    out << "quat(w=" << q.w << ", " << q.x << ", " << q.y << ", " << q.z << ')';
    return out.str();
}

// Euclidean distance between `a` and `b` at most `tol`.
template <glm::length_t L, typename T, glm::qualifier Q>
[[nodiscard]] ::testing::AssertionResult vec_near(const char* a_expr, const char* b_expr,
                                                  const char* tol_expr,
                                                  const glm::vec<L, T, Q>&  a,
                                                  const glm::vec<L, T, Q>&  b,
                                                  std::type_identity_t<T> tol) {
    const T d = glm::distance(a, b);
    if (d <= tol) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << a_expr << " = " << glm_text(a) << " is " << d << " away from " << b_expr << " = "
           << glm_text(b) << " (tolerance " << tol_expr << ")";
}

// Unit quaternions describing the same rotation: 1 - |dot(a, b)| at most `tol`.
template <typename T, glm::qualifier Q>
[[nodiscard]] ::testing::AssertionResult same_rotation(const char* a_expr, const char* b_expr,
                                                       const char* tol_expr,
                                                       const glm::qua<T, Q>&   a,
                                                       const glm::qua<T, Q>&   b,
                                                       std::type_identity_t<T> tol) {
    const T d = T(1) - glm::abs(glm::dot(a, b));
    if (d <= tol) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << a_expr << " = " << glm_text(a) << " is not the rotation " << b_expr << " = "
           << glm_text(b) << " (1 - |dot| = " << d << ", tolerance " << tol_expr << ")";
}

} // namespace quat::test

#define EXPECT_VEC_NEAR(a, b, tol) EXPECT_PRED_FORMAT3(::quat::test::vec_near, a, b, tol)
#define ASSERT_VEC_NEAR(a, b, tol) ASSERT_PRED_FORMAT3(::quat::test::vec_near, a, b, tol)
#define EXPECT_SAME_ROTATION(a, b, tol) EXPECT_PRED_FORMAT3(::quat::test::same_rotation, a, b, tol)
