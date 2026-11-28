#pragma once

// Allocation probe for memory-bound tests: engine_tests replaces the global
// operator new / delete (alloc_probe.cpp) so a test can measure the largest
// single heap allocation made inside a scope (and how many were made), e.g.
// that untrusted input is refused before anything large is allocated:
//
//   quat::test::AllocProbe probe;          // starts recording
//   (void)apply_alpha_tested_mips(...);
//   EXPECT_LT(probe.largest(), 64 * 1024);
//
// It sees operator new only (std::vector, std::string, make_unique, ...),
// not malloc. One probe at a time; record from the test's own thread.

#include <cstddef>

namespace quat::test {

class AllocProbe {
public:
    AllocProbe();
    ~AllocProbe();
    AllocProbe(const AllocProbe&) = delete;
    AllocProbe& operator=(const AllocProbe&) = delete;

    // Largest single operator-new request since construction, in bytes.
    [[nodiscard]] std::size_t largest() const;
    // Number of operator-new calls since construction.
    [[nodiscard]] std::size_t count() const;
};

} // namespace quat::test
