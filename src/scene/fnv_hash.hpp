#pragma once

// FNV-1a (64-bit) for content keys in the scene module (private header).
// Floats hash by value with -0 folded into +0, so equal values always produce
// equal keys.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace engine::scene {

class Fnv64 {
public:
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) h_ = (h_ ^ p[i]) * 1099511628211ULL;
    }
    void f32(float f) {
        f += 0.0F; // -0 -> +0
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        bytes(&bits, sizeof bits);
    }
    void u32(std::uint32_t v) { bytes(&v, sizeof v); }
    void u64(std::uint64_t v) { bytes(&v, sizeof v); }
    // Length-prefixed, so ("ab", "c") and ("a", "bc") differ.
    void str(std::string_view s) {
        u64(s.size());
        bytes(s.data(), s.size());
    }
    [[nodiscard]] std::uint64_t value() const { return h_; }
    // value(), remapped so 0 can mean "never computed".
    [[nodiscard]] std::uint64_t nonzero() const { return h_ == 0 ? 1 : h_; }

private:
    std::uint64_t h_ = 14695981039346656037ULL;
};

} // namespace engine::scene
