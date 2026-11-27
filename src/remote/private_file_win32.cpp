// write_private_file on Windows — see private_file.hpp. (Compiled only on
// Windows; guarded too so a `src/remote/*.cpp` glob builds anywhere.)
//
// The temporary file is created with CREATE_NEW (the O_EXCL counterpart) and
// FILE_FLAG_OPEN_REPARSE_POINT (never through a link), under an explicit
// protected DACL that grants only the current user's SID: an explicit
// QUAT_REMOTE_TOKEN_FILE in a shared directory (C:\Users\Public, a project
// drive) must not hand its readable ACL to the token. A same-volume
// MoveFileExW keeps that security descriptor.

#if defined(_WIN32)

#include "private_file.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include "engine/core/utf8_path.hpp"

namespace engine::remote::detail {

namespace {

namespace fs = std::filesystem;
using core::Error;
using core::path_to_utf8;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

[[nodiscard]] std::string win_error(DWORD err) { return "Windows error " + std::to_string(err); }

struct OwnedHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) : h(handle) {}
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { close(); }
    bool close() {
        const bool ok = h == INVALID_HANDLE_VALUE || h == nullptr || CloseHandle(h) != 0;
        h = INVALID_HANDLE_VALUE;
        return ok;
    }
};

struct LocalMemory {
    HLOCAL p = nullptr;
    LocalMemory() = default;
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    ~LocalMemory() {
        if (p != nullptr) LocalFree(p);
    }
};

// "D:P(A;;FA;;;<user SID>)": a protected DACL (nothing inherited from the
// directory) with one entry, full access for the current user.
[[nodiscard]] std::expected<std::wstring, Error> user_only_sddl() {
    HANDLE raw_token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == 0) {
        return fail("cannot open the process token: " + win_error(GetLastError()));
    }
    const OwnedHandle token(raw_token);
    DWORD size = 0;
    (void)GetTokenInformation(raw_token, TokenUser, nullptr, 0, &size);
    if (size == 0) return fail("cannot query the current user: " + win_error(GetLastError()));
    // TOKEN_USER needs pointer alignment.
    std::vector<std::uint64_t> buffer((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    if (GetTokenInformation(raw_token, TokenUser, buffer.data(), size, &size) == 0) {
        return fail("cannot query the current user: " + win_error(GetLastError()));
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LocalMemory sid_text;
    LPWSTR sid = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid) == 0) {
        return fail("cannot format the user SID: " + win_error(GetLastError()));
    }
    sid_text.p = sid;
    return std::wstring(L"D:P(A;;FA;;;") + sid + L")";
}

} // namespace

std::expected<void, Error> write_private_file(const fs::path& path, std::string_view content) {
    std::error_code ec;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return fail("cannot create " + path_to_utf8(parent) + ": " + ec.message());
    }
    auto sddl = user_only_sddl();
    if (!sddl) return std::unexpected(sddl.error());
    LocalMemory descriptor;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl->c_str(), SDDL_REVISION_1, &sd,
                                                             nullptr) == 0) {
        return fail("cannot build the token file's ACL: " + win_error(GetLastError()));
    }
    descriptor.p = sd;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    const fs::path tmp =
        fs::path(path).concat(L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    (void)DeleteFileW(tmp.c_str()); // stale leftover of a crashed run with our pid
    OwnedHandle file(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.h == INVALID_HANDLE_VALUE) {
        return fail("cannot create " + path_to_utf8(tmp) + ": " + win_error(GetLastError()));
    }
    std::size_t off = 0;
    DWORD err = 0;
    while (err == 0 && off < content.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - off, 1U << 20U));
        DWORD written = 0;
        if (WriteFile(file.h, content.data() + off, chunk, &written, nullptr) == 0 || written == 0) {
            err = GetLastError();
            if (err == 0) err = ERROR_WRITE_FAULT; // a zero-byte write must still end the loop
        }
        off += written;
    }
    if (!file.close() && err == 0) err = GetLastError();
    if (err != 0) {
        (void)DeleteFileW(tmp.c_str());
        return fail("cannot write " + path_to_utf8(tmp) + ": " + win_error(err));
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        0) {
        err = GetLastError();
        (void)DeleteFileW(tmp.c_str());
        return fail("cannot replace " + path_to_utf8(path) + ": " + win_error(err));
    }
    return {};
}

} // namespace engine::remote::detail

#endif // _WIN32
