#include "engine/asset/asset_manager.hpp"

#include <cstddef>
#include <string>

namespace engine::asset {

namespace {

// ---------------------------------------------------------------------------
// xxHash64 (canonical, seed 0). Hand-rolled to avoid pulling in a dependency
// for a single 64-bit non-cryptographic hash. Byte-wise reads keep it
// endian-safe and free of strict-aliasing concerns. Reference test vector:
// XXH64("") == 0xEF46DB3751D8E999.
// ---------------------------------------------------------------------------
constexpr std::uint64_t kPrime1 = 11400714785074694791ULL;
constexpr std::uint64_t kPrime2 = 14029467366897019727ULL;
constexpr std::uint64_t kPrime3 = 1609587929392839161ULL;
constexpr std::uint64_t kPrime4 = 9650029242287828579ULL;
constexpr std::uint64_t kPrime5 = 2870177450012600261ULL;

[[nodiscard]] std::uint64_t rotl(std::uint64_t x, int r) noexcept {
    return (x << r) | (x >> (64 - r));
}

[[nodiscard]] std::uint64_t read64_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(p[0]) | (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) | (static_cast<std::uint64_t>(p[3]) << 24) |
           (static_cast<std::uint64_t>(p[4]) << 32) | (static_cast<std::uint64_t>(p[5]) << 40) |
           (static_cast<std::uint64_t>(p[6]) << 48) | (static_cast<std::uint64_t>(p[7]) << 56);
}

[[nodiscard]] std::uint32_t read32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::uint64_t xxh_round(std::uint64_t acc, std::uint64_t input) noexcept {
    acc += input * kPrime2;
    acc = rotl(acc, 31);
    acc *= kPrime1;
    return acc;
}

[[nodiscard]] std::uint64_t xxh_merge_round(std::uint64_t acc, std::uint64_t val) noexcept {
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * kPrime1 + kPrime4;
    return acc;
}

[[nodiscard]] std::uint64_t xxhash64(const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint8_t* const end = p + len;
    std::uint64_t h64 = 0;

    if (len >= 32) {
        const std::uint8_t* const limit = end - 32;
        std::uint64_t v1 = kPrime1 + kPrime2;
        std::uint64_t v2 = kPrime2;
        std::uint64_t v3 = 0;
        std::uint64_t v4 = 0ULL - kPrime1;
        do {
            v1 = xxh_round(v1, read64_le(p)); p += 8;
            v2 = xxh_round(v2, read64_le(p)); p += 8;
            v3 = xxh_round(v3, read64_le(p)); p += 8;
            v4 = xxh_round(v4, read64_le(p)); p += 8;
        } while (p <= limit);

        h64 = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h64 = xxh_merge_round(h64, v1);
        h64 = xxh_merge_round(h64, v2);
        h64 = xxh_merge_round(h64, v3);
        h64 = xxh_merge_round(h64, v4);
    } else {
        h64 = kPrime5;
    }

    h64 += static_cast<std::uint64_t>(len);

    while (p + 8 <= end) {
        h64 ^= xxh_round(0, read64_le(p));
        h64 = rotl(h64, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (p + 4 <= end) {
        h64 ^= static_cast<std::uint64_t>(read32_le(p)) * kPrime1;
        h64 = rotl(h64, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end) {
        h64 ^= static_cast<std::uint64_t>(*p) * kPrime5;
        h64 = rotl(h64, 11) * kPrime1;
        ++p;
    }

    h64 ^= h64 >> 33;
    h64 *= kPrime2;
    h64 ^= h64 >> 29;
    h64 *= kPrime3;
    h64 ^= h64 >> 32;
    return h64;
}

} // namespace

AssetId asset_id_from_path(std::string_view relative_path) {
    // Normalise so "a/b.gltf", "./a/b.gltf", and "a\\b.gltf" map to one id.
    std::filesystem::path path = std::filesystem::path(relative_path).lexically_normal();
    std::string canonical = path.generic_string();
    return AssetId{xxhash64(canonical.data(), canonical.size())};
}

} // namespace engine::asset
