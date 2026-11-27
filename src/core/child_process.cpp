#include "engine/core/child_process.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ; // NOLINT: POSIX-defined process environment
#endif

namespace engine::core {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

} // namespace

#if defined(_WIN32)

namespace {

[[nodiscard]] std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// Quotes one argument following the MSVC runtime's command-line parsing rules.
void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += arg[i];
    }
    cmd += L'"';
}

} // namespace

std::expected<ProcessResult, Error>
run_process(const fs::path& program, std::span<const std::string> args) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return fail("CreatePipe failed");
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0); // parent-only end

    std::wstring cmd;
    append_quoted(cmd, program.wstring());
    for (const std::string& a : args) {
        cmd += L' ';
        append_quoted(cmd, widen(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    const std::wstring app = program.wstring();
    const BOOL started = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end); // the child holds its own copy
    if (!started) {
        CloseHandle(read_end);
        return fail("CreateProcess failed for " + program.string());
    }

    ProcessResult result;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        result.output.append(buffer, got);
    }
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    result.exit_code = static_cast<int>(code);
    return result;
}

#else // POSIX

std::expected<ProcessResult, Error>
run_process(const fs::path& program, std::span<const std::string> args) {
    // posix_spawn reports a failed exec only on some implementations (glibc
    // does; others spawn a child that exits 127), so check up front.
    if (::access(program.c_str(), X_OK) != 0) {
        return fail("cannot start " + program.string() + ": " + std::strerror(errno));
    }
    int pipe_fds[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return fail(std::string("pipe2 failed: ") + std::strerror(errno));
    }
#else
    if (::pipe(pipe_fds) != 0) {
        return fail(std::string("pipe failed: ") + std::strerror(errno));
    }
    ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
#endif

    // argv: program, args..., null. posix_spawn takes char* const[] but does
    // not modify the strings.
    const std::string program_str = program.string();
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program_str.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // stdout and stderr both go to the pipe (dup2 clears close-on-exec).
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);

    pid_t pid = -1;
    const int spawn_err =
        ::posix_spawn(&pid, program_str.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]); // the child holds its own copy
    if (spawn_err != 0) {
        ::close(pipe_fds[0]);
        return fail("cannot start " + program_str + ": " + std::strerror(spawn_err));
    }

    ProcessResult result;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.output.append(buffer, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break; // EOF: every writer (the child and its children) closed
        }
    }
    ::close(pipe_fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return fail(std::string("waitpid failed: ") + std::strerror(errno));
        }
    }
    if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

#endif

std::optional<fs::path> find_on_path(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return std::nullopt;
#if defined(_WIN32)
    constexpr char separator = ';';
    const std::string file = std::string(name) + ".exe";
#else
    constexpr char separator = ':';
    const std::string file(name);
#endif
    const std::string_view paths(path_env);
    std::size_t start = 0;
    while (start <= paths.size()) {
        std::size_t end = paths.find(separator, start);
        if (end == std::string_view::npos) end = paths.size();
        const std::string_view dir = paths.substr(start, end - start);
        if (!dir.empty()) {
            const fs::path candidate = fs::path(std::string(dir)) / file;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) {
#if defined(_WIN32)
                return candidate;
#else
                if (::access(candidate.c_str(), X_OK) == 0) return candidate;
#endif
            }
        }
        start = end + 1;
    }
    return std::nullopt;
}

} // namespace engine::core
