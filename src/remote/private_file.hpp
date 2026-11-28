#pragma once

// Writing a file only the current user can read (private to engine_remote;
// the session token file, session_token.cpp). One implementation per
// platform: private_file_posix.cpp and private_file_win32.cpp, each compiled
// only on its platform.
//
// Both write a temporary file created exclusively (a pid-unique name, never
// following a link at that name) with user-only permissions from the start,
// then rename it over `path`: readers never see a partial file, and an
// existing file or link at `path`, whatever its permissions, is replaced
// rather than written through.
//   POSIX:   the parent directory is created 0700 when missing; an existing
//            one must belong to the user (or root) and must not be writable
//            by others unless sticky. The file is 0600 regardless of umask.
//   Windows: the file gets an explicit protected DACL granting only the
//            current user's SID, whatever the directory would pass on.
// Error messages name paths in UTF-8 and carry the OS error.

#include <expected>
#include <filesystem>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::remote::detail {

[[nodiscard]] std::expected<void, core::Error> write_private_file(const std::filesystem::path& path,
                                                                  std::string_view content);

} // namespace engine::remote::detail
