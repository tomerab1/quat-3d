// Session tokens and the user-only token file — see
// include/engine/remote/session_token.hpp.

#include "engine/remote/session_token.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

#include <openssl/rand.h>

#include "engine/core/utf8_path.hpp"
#include "private_file.hpp"

namespace engine::remote {

namespace {

namespace fs = std::filesystem;
using core::Error;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// An environment variable as UTF-8 (empty when unset). On Windows the narrow
// getenv() would convert through the ANSI code page and mangle a profile path
// it cannot represent, so the wide value is read and converted instead.
[[nodiscard]] std::string env_or_empty(const char* name) {
#if defined(_WIN32)
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const wchar_t* v = _wgetenv(wide_name.c_str());
    return v != nullptr ? core::path_to_utf8(fs::path(v)) : std::string();
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

[[nodiscard]] fs::path utf8_path(const std::string& v) { return core::path_from_utf8(v); }

// XDG base directories must be absolute; relative values are ignored.
[[nodiscard]] bool usable_dir(const std::string& v) {
    return !v.empty() && utf8_path(v).is_absolute();
}

[[nodiscard]] std::string file_name(std::uint16_t port) {
    return "remote-" + std::to_string(port) + ".token";
}

} // namespace

std::expected<std::string, Error> generate_session_token() {
    std::array<unsigned char, session_token_bytes> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return fail("the system random number generator is unavailable (OpenSSL RAND_bytes "
                    "failed)");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes) {
        token.push_back(hex[b >> 4U]);
        token.push_back(hex[b & 0xFU]);
    }
    return token;
}

TokenPathEnv TokenPathEnv::from_process() {
    TokenPathEnv env;
    env.token_file = env_or_empty("QUAT_REMOTE_TOKEN_FILE");
    env.xdg_runtime_dir = env_or_empty("XDG_RUNTIME_DIR");
    env.xdg_state_home = env_or_empty("XDG_STATE_HOME");
    env.home = env_or_empty("HOME");
    env.local_app_data = env_or_empty("LOCALAPPDATA");
    return env;
}

std::expected<fs::path, Error> token_file_path(std::uint16_t port, const TokenPathEnv& env) {
    if (!env.token_file.empty()) return utf8_path(env.token_file);
#if defined(_WIN32)
    if (!usable_dir(env.local_app_data)) {
        return fail("LOCALAPPDATA is not set; set QUAT_REMOTE_TOKEN_FILE");
    }
    return utf8_path(env.local_app_data) / "quat-3d" / file_name(port);
#elif defined(__APPLE__)
    if (!usable_dir(env.home)) return fail("HOME is not set; set QUAT_REMOTE_TOKEN_FILE");
    return utf8_path(env.home) / "Library" / "Application Support" / "quat-3d" / file_name(port);
#else
    if (usable_dir(env.xdg_runtime_dir)) {
        return utf8_path(env.xdg_runtime_dir) / "quat-3d" / file_name(port);
    }
    if (usable_dir(env.xdg_state_home)) {
        return utf8_path(env.xdg_state_home) / "quat-3d" / file_name(port);
    }
    if (!usable_dir(env.home)) {
        return fail("neither XDG_RUNTIME_DIR nor HOME is set; set QUAT_REMOTE_TOKEN_FILE");
    }
    return utf8_path(env.home) / ".local" / "state" / "quat-3d" / file_name(port);
#endif
}

std::expected<SessionTokenFile, Error> SessionTokenFile::publish(fs::path path,
                                                                 std::string_view token) {
    if (path.empty()) return fail("empty token file path");
    std::string content(token);
    content.push_back('\n');
    auto written = detail::write_private_file(path, content);
    if (!written) return std::unexpected(written.error());
    return SessionTokenFile(std::move(path));
}

SessionTokenFile::SessionTokenFile(SessionTokenFile&& other) noexcept
    : path_(std::exchange(other.path_, {})) {}

SessionTokenFile& SessionTokenFile::operator=(SessionTokenFile&& other) noexcept {
    if (this != &other) {
        remove();
        path_ = std::exchange(other.path_, {});
    }
    return *this;
}

SessionTokenFile::~SessionTokenFile() { remove(); }

void SessionTokenFile::remove() {
    if (path_.empty()) return;
    std::error_code ignored;
    fs::remove(path_, ignored);
    path_.clear();
}

} // namespace engine::remote
