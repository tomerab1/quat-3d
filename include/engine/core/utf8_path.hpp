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

#include <filesystem>
#include <string>
#include <string_view>

namespace engine::core {

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
