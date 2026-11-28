#pragma once

// UTF-8 <-> std::filesystem::path at the boundaries.
//
// Engine strings that hold paths (JSON fields, SDL/ImGui text, pipeline-cache
// keys, child-process arguments) are UTF-8. Constructing a path from a narrow
// string — fs::path(std::string) — or reading one back with .string() uses
// the *native narrow* encoding, which on Windows is the ANSI code page: a
// non-ASCII path is mangled, and .string() aborts on characters the code page
// cannot represent (the engine builds without exceptions). Convert through
// these instead; on Linux/macOS they are the identity on the bytes.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace engine::core {

// Well-formed UTF-8 (RFC 3629): no stray continuation bytes, truncated
// sequences, overlong forms, surrogates or code points above U+10FFFF.
// Check untrusted text before path_from_utf8: on Windows the conversion
// throws (the engine builds without exceptions, so it aborts) on bad input.
[[nodiscard]] inline bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80U) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        unsigned char lo = 0x80U; // allowed range of the second byte
        unsigned char hi = 0xBFU;
        if (b0 >= 0xC2U && b0 <= 0xDFU) {
            len = 2;
        } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
            len = 3;
            if (b0 == 0xE0U) lo = 0xA0U; // overlong
            if (b0 == 0xEDU) hi = 0x9FU; // surrogates
        } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
            len = 4;
            if (b0 == 0xF0U) lo = 0x90U; // overlong
            if (b0 == 0xF4U) hi = 0x8FU; // above U+10FFFF
        } else {
            return false; // continuation byte, C0 / C1 overlong lead, F5..FF
        }
        if (s.size() - i < len) return false;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if (b1 < lo || b1 > hi) return false;
        for (std::size_t k = 2; k < len; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if (b < 0x80U || b > 0xBFU) return false;
        }
        i += len;
    }
    return true;
}

[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// Same, with '/' separators on every platform (cache keys, messages).
[[nodiscard]] inline std::string path_to_generic_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

} // namespace engine::core
