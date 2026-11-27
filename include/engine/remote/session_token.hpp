#pragma once

// Session tokens for the remote-control endpoint (issue #56).
//
// The endpoint never runs unauthenticated: when QUAT_REMOTE_TOKEN is not
// given, the engine generates a random 256-bit token and publishes it in a
// per-user token file that only the user can read. quat-mcp reads the same
// file, so the zero-config workflow (`QUAT_REMOTE=1 ./game` plus an MCP
// client) keeps working while other local users and web pages cannot connect.
//
// Token file location (docs/remote-control.md, "Session token"); quat-mcp
// resolves the same path:
//   QUAT_REMOTE_TOKEN_FILE        set: exactly that path
//   Windows                       %LOCALAPPDATA%\quat-3d\remote-<port>.token
//   macOS                         ~/Library/Application Support/quat-3d/remote-<port>.token
//   Linux / other POSIX           $XDG_RUNTIME_DIR/quat-3d/remote-<port>.token,
//                                 else $XDG_STATE_HOME (default ~/.local/state)/quat-3d/...
// The directory is created user-only (0700) and the file is written 0600.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::remote {

inline constexpr std::size_t session_token_bytes = 32; // 256 bits

// A fresh token: session_token_bytes from OpenSSL's CSPRNG, hex-encoded
// (64 lowercase characters).
[[nodiscard]] std::expected<std::string, core::Error> generate_session_token();

// The environment the token file path depends on. Empty strings mean unset.
struct TokenPathEnv {
    std::string token_file;      // QUAT_REMOTE_TOKEN_FILE
    std::string xdg_runtime_dir; // XDG_RUNTIME_DIR
    std::string xdg_state_home;  // XDG_STATE_HOME
    std::string home;            // HOME
    std::string local_app_data;  // LOCALAPPDATA (Windows)

    [[nodiscard]] static TokenPathEnv from_process();
};

// Where the token for an endpoint on `port` is published (rules above).
// Relative XDG values are ignored, as the XDG spec requires. Fails when no
// per-user directory can be determined (no HOME / LOCALAPPDATA).
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
token_file_path(std::uint16_t port, const TokenPathEnv& env);

// A published token file. publish() writes the token atomically (temporary
// file + rename) with user-only permissions, creating the parent directory
// user-only when missing; on POSIX it refuses a parent directory owned by
// another user. The file is removed when the object is destroyed. Move-only.
class SessionTokenFile {
public:
    [[nodiscard]] static std::expected<SessionTokenFile, core::Error>
    publish(std::filesystem::path path, std::string_view token);

    SessionTokenFile() = default;
    SessionTokenFile(SessionTokenFile&& other) noexcept;
    SessionTokenFile& operator=(SessionTokenFile&& other) noexcept;
    SessionTokenFile(const SessionTokenFile&) = delete;
    SessionTokenFile& operator=(const SessionTokenFile&) = delete;
    ~SessionTokenFile();

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    explicit SessionTokenFile(std::filesystem::path path) : path_(std::move(path)) {}
    void remove();

    std::filesystem::path path_; // empty = nothing to remove
};

} // namespace engine::remote
