// Global operator new / delete for engine_tests, forwarding to malloc / free
// and recording the largest request (and the request count) while an
// AllocProbe is alive.

#include "support/alloc_probe.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool>        g_recording{false};
std::atomic<std::size_t> g_largest{0};
std::atomic<std::size_t> g_count{0};

void* allocate(std::size_t size) noexcept {
    if (g_recording.load(std::memory_order_relaxed)) {
        g_count.fetch_add(1, std::memory_order_relaxed);
        std::size_t seen = g_largest.load(std::memory_order_relaxed);
        while (size > seen &&
               !g_largest.compare_exchange_weak(seen, size, std::memory_order_relaxed)) {
        }
    }
    return std::malloc(size == 0 ? 1 : size);
}

} // namespace

namespace quat::test {

AllocProbe::AllocProbe() {
    g_largest.store(0, std::memory_order_relaxed);
    g_count.store(0, std::memory_order_relaxed);
    g_recording.store(true, std::memory_order_relaxed);
}

AllocProbe::~AllocProbe() { g_recording.store(false, std::memory_order_relaxed); }

std::size_t AllocProbe::largest() const { return g_largest.load(std::memory_order_relaxed); }

std::size_t AllocProbe::count() const { return g_count.load(std::memory_order_relaxed); }

} // namespace quat::test

// The engine builds without exceptions: an exhausted heap terminates, as the
// library's operator new would.
void* operator new(std::size_t size) {
    void* p = allocate(size);
    if (p == nullptr) std::abort();
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
