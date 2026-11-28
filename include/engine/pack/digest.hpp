#pragma once

// SHA-256 digests for the package manifest (ROADMAP P4), computed with
// OpenSSL's EVP API (OpenSSL is already in the dependency tree through
// GameNetworkingSockets; engine_pack links OpenSSL::Crypto explicitly).
// Streaming: update() any number of times, then hex().

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::pack {

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;

    void update(std::string_view data);
    // Finalises the digest as 64 lower-case hex digits; the object is spent.
    // Fails only if OpenSSL does (no SHA-256 provider, out of memory).
    [[nodiscard]] std::expected<std::string, core::Error> hex();

private:
    struct Context;
    std::unique_ptr<Context> ctx_;
    bool                     ok_ = true;
};

[[nodiscard]] std::expected<std::string, core::Error> sha256_hex(std::string_view data);

// Digest of a file's contents (read in chunks); its size through `bytes`.
[[nodiscard]] std::expected<std::string, core::Error>
sha256_file(const std::filesystem::path& path, std::uint64_t& bytes);

} // namespace engine::pack
