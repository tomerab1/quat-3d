#pragma once

// SHA-256 (FIPS 180-4) for the package manifest's file digests (ROADMAP P4).
// Streaming: update() any number of times, then hex().

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::pack {

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    void update(std::string_view data);
    // Finalises and returns the 32-byte digest; the object is spent.
    [[nodiscard]] std::array<std::uint8_t, 32> digest();
    // Finalises and returns the digest as 64 lower-case hex digits.
    [[nodiscard]] std::string hex();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t                  buf_len_ = 0;
    std::uint64_t                total_ = 0;
};

[[nodiscard]] std::string sha256_hex(std::string_view data);

// Digest of a file's contents (read in chunks); size returned through `bytes`.
[[nodiscard]] std::expected<std::string, core::Error>
sha256_file(const std::filesystem::path& path, std::uint64_t& bytes);

} // namespace engine::pack
