#include "engine/core/env.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
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

std::optional<bool> parse_env_flag(std::string_view value) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && blank(value.front())) value.remove_prefix(1);
    while (!value.empty() && blank(value.back())) value.remove_suffix(1);
    std::string lower(value);
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    constexpr std::array<std::string_view, 4> on{"1", "true", "yes", "on"};
    constexpr std::array<std::string_view, 5> off{"", "0", "false", "no", "off"};
    if (std::ranges::find(on, lower) != on.end()) return true;
    if (std::ranges::find(off, lower) != off.end()) return false;
    return std::nullopt;
}

bool env_flag(const char* name, bool fallback) {
    const std::optional<std::string> value = env_utf8(name);
    if (!value) return fallback;
    if (const std::optional<bool> flag = parse_env_flag(*value)) return *flag;
    std::fprintf(stderr, "[warn] %s=%s is not a boolean (1/0, true/false, yes/no, on/off); "
                         "treating it as on\n",
                 name, value->c_str());
    return true;
}

} // namespace engine::core
