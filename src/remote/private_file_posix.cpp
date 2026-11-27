// write_private_file on POSIX — see private_file.hpp. (Compiled only off
// Windows; guarded too so a `src/remote/*.cpp` glob builds anywhere.)

#if !defined(_WIN32)

#include "private_file.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/core/utf8_path.hpp"

namespace engine::remote::detail {

namespace {

namespace fs = std::filesystem;
using core::Error;
using core::path_to_utf8;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

[[nodiscard]] std::string errno_text(int err) { return std::strerror(err); }

// The file's directory: created user-only when missing; an existing one must
// not let other users swap the file (owned by us or root, and not writable by
// group/others unless sticky, like /tmp).
[[nodiscard]] std::expected<void, Error> ensure_private_dir(const fs::path& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    if (const fs::path parent = dir.parent_path(); !parent.empty() && parent != dir) {
        fs::create_directories(parent, ec);
        if (ec) return fail("cannot create " + path_to_utf8(parent) + ": " + ec.message());
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return fail("cannot create " + path_to_utf8(dir) + ": " + errno_text(errno));
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        return fail("cannot inspect " + path_to_utf8(dir) + ": " + errno_text(errno));
    }
    if (!S_ISDIR(st.st_mode)) return fail(path_to_utf8(dir) + " is not a directory");
    if (st.st_uid != ::geteuid() && st.st_uid != 0) {
        return fail(path_to_utf8(dir) + " belongs to another user");
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (st.st_mode & S_ISVTX) == 0) {
        return fail(path_to_utf8(dir) + " is writable by other users");
    }
    return {};
}

} // namespace

std::expected<void, Error> write_private_file(const fs::path& path, std::string_view content) {
    auto dir = ensure_private_dir(path.parent_path());
    if (!dir) return std::unexpected(dir.error());

    const fs::path tmp = fs::path(path).concat("." + std::to_string(::getpid()) + ".tmp");
    (void)::unlink(tmp.c_str()); // stale leftover of a crashed run with our pid
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail("cannot create " + path_to_utf8(tmp) + ": " + errno_text(errno));
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
        return fail("cannot write " + path_to_utf8(tmp) + ": " + errno_text(write_errno));
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        (void)::unlink(tmp.c_str());
        return fail("cannot replace " + path_to_utf8(path) + ": " + errno_text(err));
    }
    return {};
}

} // namespace engine::remote::detail

#endif // !_WIN32
