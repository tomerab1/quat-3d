#include "engine/core/env.hpp"

#include <cstdlib>
#include <cstring>

#include "engine/core/utf8_path.hpp"

namespace engine::core {

namespace {

namespace fs = std::filesystem;

// The raw value in the platform's native path encoding (UTF-16 on Windows,
// bytes elsewhere), as a path; nullopt when unset.
[[nodiscard]] std::optional<fs::path> native_value(const char* name) {
#if defined(_WIN32)
    // Variable names are ASCII; widen byte by byte.
    const std::wstring wide_name(name, name + std::strlen(name));
    const wchar_t* value = ::_wgetenv(wide_name.c_str());
    if (value == nullptr) return std::nullopt;
    return fs::path(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return fs::path(value);
#endif
}

} // namespace

std::optional<std::string> env_utf8(const char* name) {
    auto value = native_value(name);
    if (!value) return std::nullopt;
    return path_to_utf8(*value);
}

std::optional<fs::path> env_path(const char* name) {
    auto value = native_value(name);
    if (!value || value->empty()) return std::nullopt;
    return value;
}

} // namespace engine::core
