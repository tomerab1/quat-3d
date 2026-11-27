// Session tokens and the user-only token file — see
// include/engine/remote/session_token.hpp.

#include "engine/remote/session_token.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#include <openssl/rand.h>

#if defined(_WIN32)
#include <stdio.h> // _wfopen_s
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace engine::remote {

namespace {

namespace fs = std::filesystem;
using core::Error;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

[[nodiscard]] std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

// XDG base directories must be absolute; relative values are ignored.
[[nodiscard]] bool usable_dir(const std::string& v) {
    return !v.empty() && fs::path(v).is_absolute();
}

[[nodiscard]] std::string file_name(std::uint16_t port) {
    return "remote-" + std::to_string(port) + ".token";
}

#if !defined(_WIN32)

[[nodiscard]] std::string errno_text(int err) { return std::strerror(err); }

// The token's directory: created user-only when missing; an existing one must
// not let other users swap the file (owned by us or root, and not writable by
// group/others unless sticky, like /tmp).
[[nodiscard]] std::expected<void, Error> ensure_private_dir(const fs::path& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    if (const fs::path parent = dir.parent_path(); !parent.empty() && parent != dir) {
        fs::create_directories(parent, ec);
        if (ec) return fail("cannot create " + parent.string() + ": " + ec.message());
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return fail("cannot create " + dir.string() + ": " + errno_text(errno));
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        return fail("cannot inspect " + dir.string() + ": " + errno_text(errno));
    }
    if (!S_ISDIR(st.st_mode)) return fail(dir.string() + " is not a directory");
    if (st.st_uid != ::geteuid() && st.st_uid != 0) {
        return fail(dir.string() + " belongs to another user");
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (st.st_mode & S_ISVTX) == 0) {
        return fail(dir.string() + " is writable by other users");
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> write_private_file(const fs::path& path,
                                                            std::string_view content) {
    auto dir = ensure_private_dir(path.parent_path());
    if (!dir) return std::unexpected(dir.error());

    // Write a temporary file and rename it over the target: readers never see
    // a partial token, and an existing file (or symlink) with looser
    // permissions is replaced rather than written through.
    const fs::path tmp = fs::path(path).concat("." + std::to_string(::getpid()) + ".tmp");
    (void)::unlink(tmp.c_str()); // stale leftover of a crashed run with our pid
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail("cannot create " + tmp.string() + ": " + errno_text(errno));
    bool ok = ::fchmod(fd, 0600) == 0; // umask can only clear bits; make it exact
    std::size_t off = 0;
    while (ok && off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) off += static_cast<std::size_t>(n);
    }
    const int write_errno = errno;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        (void)::unlink(tmp.c_str());
        return fail("cannot write " + tmp.string() + ": " + errno_text(write_errno));
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        (void)::unlink(tmp.c_str());
        return fail("cannot replace " + path.string() + ": " + errno_text(err));
    }
    return {};
}

#else // _WIN32

// %LOCALAPPDATA% is per-user (its ACL grants only the user, SYSTEM and
// administrators), and files created in it inherit that ACL.
[[nodiscard]] std::expected<void, Error> write_private_file(const fs::path& path,
                                                            std::string_view content) {
    std::error_code ec;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return fail("cannot create " + parent.string() + ": " + ec.message());
    }
    const fs::path tmp = fs::path(path).concat(L".tmp");
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || f == nullptr) {
        return fail("cannot create " + tmp.string());
    }
    const bool written = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed) {
        fs::remove(tmp, ec);
        return fail("cannot write " + tmp.string());
    }
    fs::rename(tmp, path, ec); // replaces an existing file
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return fail("cannot replace " + path.string() + ": " + ec.message());
    }
    return {};
}

#endif

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
    if (!env.token_file.empty()) return fs::path(env.token_file);
#if defined(_WIN32)
    if (!usable_dir(env.local_app_data)) {
        return fail("LOCALAPPDATA is not set; set QUAT_REMOTE_TOKEN_FILE");
    }
    return fs::path(env.local_app_data) / "quat-3d" / file_name(port);
#elif defined(__APPLE__)
    if (!usable_dir(env.home)) return fail("HOME is not set; set QUAT_REMOTE_TOKEN_FILE");
    return fs::path(env.home) / "Library" / "Application Support" / "quat-3d" / file_name(port);
#else
    if (usable_dir(env.xdg_runtime_dir)) {
        return fs::path(env.xdg_runtime_dir) / "quat-3d" / file_name(port);
    }
    if (usable_dir(env.xdg_state_home)) {
        return fs::path(env.xdg_state_home) / "quat-3d" / file_name(port);
    }
    if (!usable_dir(env.home)) {
        return fail("neither XDG_RUNTIME_DIR nor HOME is set; set QUAT_REMOTE_TOKEN_FILE");
    }
    return fs::path(env.home) / ".local" / "state" / "quat-3d" / file_name(port);
#endif
}

std::expected<SessionTokenFile, Error> SessionTokenFile::publish(fs::path path,
                                                                 std::string_view token) {
    if (path.empty()) return fail("empty token file path");
    std::string content(token);
    content.push_back('\n');
    auto written = write_private_file(path, content);
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
