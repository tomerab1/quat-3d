#pragma once

// Row-parallel loop for the CPU terrain generator: runs body(z) for every
// z in [0, rows) across the hardware threads (interleaved rows, so the cost
// of uneven rows spreads evenly). Blocks until every row is done.

#include <cstdint>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

namespace engine::terrain::detail {

template <typename Body>
void parallel_rows(std::uint32_t rows, const Body& body) {
    const std::uint32_t workers =
        glm::clamp(std::thread::hardware_concurrency(), 1U, glm::clamp(rows, 1U, 16U));
    if (workers <= 1) {
        for (std::uint32_t z = 0; z < rows; ++z) body(z);
        return;
    }
    // std::thread + explicit join rather than std::jthread: libc++ 18
    // (Ubuntu 24.04's clang, our CI) still gates jthread behind
    // -fexperimental-library.
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::uint32_t w = 0; w < workers; ++w) {
        threads.emplace_back([&body, rows, workers, w] {
            for (std::uint32_t z = w; z < rows; z += workers) body(z);
        });
    }
    for (std::thread& t : threads) t.join();
}

} // namespace engine::terrain::detail
