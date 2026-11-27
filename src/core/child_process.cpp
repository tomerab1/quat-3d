#include "engine/core/child_process.hpp"
#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"

#include <algorithm>
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
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ; // NOLINT: POSIX-defined process environment
#endif

namespace engine::core {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// Longest a wait loop sleeps between checks of the canceller and deadline.
constexpr std::chrono::milliseconds k_wait_slice{50};

// Why run_process stopped waiting before the child exited on its own.
enum class Abort : std::uint8_t { none, timed_out, cancelled, failed };

[[nodiscard]] std::unexpected<Error> abort_error(Abort why, const fs::path& program,
                                                 const ProcessOptions& options,
                                                 const std::string& output) {
    std::string what = core::path_to_utf8(program.filename());
    if (why == Abort::timed_out) {
        what += " timed out after " + std::to_string(options.timeout.count()) +
                " ms and was killed";
    } else if (why == Abort::cancelled) {
        what += " was cancelled";
    } else {
        what += ": waiting for it failed; it was killed";
    }
    if (!output.empty()) what += "; output so far:\n" + output;
    return fail(std::move(what));
}

// When `options.timeout` expires (max() when there is none).
[[nodiscard]] Clock::time_point deadline_for(const ProcessOptions& options) {
    return options.timeout.count() > 0 ? Clock::now() + options.timeout
                                       : Clock::time_point::max();
}

// How long to block before re-checking the canceller / deadline; false once
// the deadline has passed.
[[nodiscard]] bool next_wait(Clock::time_point deadline, std::chrono::milliseconds& wait) {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) return false;
    const auto slice = std::min<Clock::duration>(deadline - now, k_wait_slice);
    wait = std::chrono::ceil<std::chrono::milliseconds>(slice);
    return true;
}

void kill_child(std::intptr_t child);

} // namespace

namespace detail {

// run_process's access to a ProcessCanceller.
struct CancellerAccess {
    // Registers the running child; false when the canceller was already
    // cancelled (the caller then kills the child itself).
    [[nodiscard]] static bool attach(ProcessCanceller* c, std::intptr_t child) {
        if (c == nullptr) return true;
        std::lock_guard lock(c->mutex_);
        if (c->cancelled_) return false;
        c->child_ = child;
        c->has_child_ = true;
        return true;
    }
    // Unregisters the child; called before it is reaped / its handle closed.
    static void detach(ProcessCanceller* c) {
        if (c == nullptr) return;
        std::lock_guard lock(c->mutex_);
        c->has_child_ = false;
        c->child_ = 0;
    }
    static void cancel(ProcessCanceller& c) {
        std::lock_guard lock(c.mutex_);
        c.cancelled_ = true;
        if (c.has_child_) kill_child(c.child_);
    }
};

} // namespace detail

void ProcessCanceller::cancel() { detail::CancellerAccess::cancel(*this); }

bool ProcessCanceller::cancelled() const {
    std::lock_guard lock(mutex_);
    return cancelled_;
}

#if defined(_WIN32)

namespace {

void kill_child(std::intptr_t child) {
    // Terminating a process that already exited fails harmlessly; the handle
    // stays open until run_process has unregistered it.
    TerminateProcess(reinterpret_cast<HANDLE>(child), 1);
}

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
run_process(const fs::path& program, std::span<const std::string> args,
            const ProcessOptions& options) {
    // Both pipe ends start non-inheritable. The child's end, and a NUL stdin,
    // become inheritable only for the CreateProcess call, and that call
    // inherits exactly those two (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) — so a
    // concurrent run_process never hands its pipe to another call's child.
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, nullptr, 0)) {
        return fail("CreatePipe failed (error " + std::to_string(GetLastError()) + ")");
    }
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto close_all = [&] {
        CloseHandle(read_end);
        CloseHandle(write_end);
        if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    };
    if (null_in == INVALID_HANDLE_VALUE ||
        !SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        const DWORD err = GetLastError();
        close_all();
        return fail("cannot set up the child's standard handles (error " + std::to_string(err) +
                    ")");
    }

    HANDLE inherited[] = {write_end, null_in};
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size); // sizes the list
    std::vector<std::uint64_t> list_storage((list_size + sizeof(std::uint64_t) - 1) /
                                            sizeof(std::uint64_t));
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &list_size)) {
        const DWORD err = GetLastError();
        close_all();
        return fail("InitializeProcThreadAttributeList failed (error " + std::to_string(err) + ")");
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr)) {
        const DWORD err = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        close_all();
        return fail("UpdateProcThreadAttribute failed (error " + std::to_string(err) + ")");
    }

    std::wstring cmd;
    append_quoted(cmd, program.wstring());
    for (const std::string& a : args) {
        cmd += L' ';
        append_quoted(cmd, widen(a));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = null_in;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError = write_end;
    si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi{};
    const std::wstring app = program.wstring();
    const BOOL started = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &si.StartupInfo, &pi);
    const DWORD create_error = started ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(write_end); // the child holds its own copies
    write_end = nullptr;
    CloseHandle(null_in);
    null_in = INVALID_HANDLE_VALUE;
    if (!started) {
        CloseHandle(read_end);
        return fail("CreateProcess failed for " + core::path_to_utf8(program) + " (error " +
                    std::to_string(create_error) + ")");
    }
    CloseHandle(pi.hThread);

    Abort abort = detail::CancellerAccess::attach(
                      options.canceller, reinterpret_cast<std::intptr_t>(pi.hProcess))
                      ? Abort::none
                      : Abort::cancelled;
    const Clock::time_point deadline = deadline_for(options);

    // Anonymous pipes have no overlapped I/O: read what is buffered, then wait
    // on the process for a slice. Once it has exited, drain what is left (a
    // grandchild still holding the pipe open is not waited for).
    ProcessResult result;
    char buffer[4096];
    const auto drain = [&] {
        DWORD available = 0;
        DWORD got = 0;
        while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) &&
               available > 0 &&
               ReadFile(read_end, buffer, std::min<DWORD>(available, sizeof(buffer)), &got,
                        nullptr) &&
               got > 0) {
            result.output.append(buffer, got);
        }
    };
    while (abort == Abort::none) {
        drain();
        if (options.canceller != nullptr && options.canceller->cancelled()) {
            abort = Abort::cancelled;
            break;
        }
        std::chrono::milliseconds wait{0};
        if (!next_wait(deadline, wait)) {
            abort = Abort::timed_out;
            break;
        }
        const DWORD waited = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(wait.count()));
        if (waited == WAIT_OBJECT_0) {
            drain();
            break;
        }
        if (waited == WAIT_FAILED) abort = Abort::failed;
    }
    // cancel() terminates the child, which ends the loop: still a cancel.
    if (abort == Abort::none && options.canceller != nullptr && options.canceller->cancelled()) {
        abort = Abort::cancelled;
    }
    if (abort != Abort::none) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    detail::CancellerAccess::detach(options.canceller); // before the handle closes
    CloseHandle(read_end);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (abort != Abort::none) return abort_error(abort, program, options, result.output);
    result.exit_code = static_cast<int>(code);
    return result;
}

#else // POSIX

namespace {

void kill_child(std::intptr_t child) {
    // The child leads its own process group: kill it with everything it
    // started. The group id is the child's pid, which cannot be reused before
    // run_process reaps it — and it is unregistered before that.
    ::kill(-static_cast<pid_t>(child), SIGKILL);
}

} // namespace

std::expected<ProcessResult, Error>
run_process(const fs::path& program, std::span<const std::string> args,
            const ProcessOptions& options) {
    // posix_spawn reports a failed exec only on some implementations (glibc
    // does; others spawn a child that exits 127), so check up front.
    if (::access(program.c_str(), X_OK) != 0) {
        return fail("cannot start " + program.string() + ": " + std::strerror(errno));
    }
    // argv: program, args..., null. posix_spawn takes char* const[] but does
    // not modify the strings.
    const std::string program_str = program.string();
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program_str.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // A process group of its own, so a timeout or cancel also kills whatever
    // the child started (a grandchild would otherwise keep the pipe open).
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short spawn_flags = POSIX_SPAWN_SETPGROUP;
#if defined(__APPLE__)
    // Only the descriptors set up below reach the child, whatever else the
    // process has open without close-on-exec.
    spawn_flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, spawn_flags);
    posix_spawnattr_setpgroup(&attr, 0);

    int pipe_fds[2] = {-1, -1};
    pid_t pid = -1;
    int spawn_err = 0;
    {
        // The pipe must be close-on-exec from birth, or a concurrent spawn
        // inherits its write end (and this call then waits for that unrelated
        // child to exit before it sees EOF). pipe2 does it atomically; without
        // it, run_process calls are serialised from pipe() until the write end
        // is closed (a fork/exec elsewhere in the process is not covered).
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
        if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
            posix_spawnattr_destroy(&attr);
            return fail(std::string("pipe2 failed: ") + std::strerror(errno));
        }
#else
        static std::mutex spawn_mutex;
        std::lock_guard spawn_lock(spawn_mutex);
        if (::pipe(pipe_fds) != 0) {
            posix_spawnattr_destroy(&attr);
            return fail(std::string("pipe failed: ") + std::strerror(errno));
        }
        ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
#endif
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        // stdin from /dev/null (a compiler never waits on the engine's
        // terminal); stdout and stderr both go to the pipe (dup2 clears
        // close-on-exec).
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
        spawn_err = ::posix_spawn(&pid, program_str.c_str(), &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipe_fds[1]); // the child holds its own copy
    }
    posix_spawnattr_destroy(&attr);
    if (spawn_err != 0) {
        ::close(pipe_fds[0]);
        return fail("cannot start " + program_str + ": " + std::strerror(spawn_err));
    }

    Abort abort = detail::CancellerAccess::attach(options.canceller, pid) ? Abort::none
                                                                          : Abort::cancelled;
    const Clock::time_point deadline = deadline_for(options);
    const bool sliced = options.canceller != nullptr || deadline != Clock::time_point::max();

    ProcessResult result;
    char buffer[4096];
    while (abort == Abort::none) {
        if (options.canceller != nullptr && options.canceller->cancelled()) {
            abort = Abort::cancelled;
            break;
        }
        int wait_ms = -1; // block until output or EOF
        if (sliced) {
            std::chrono::milliseconds wait{0};
            if (!next_wait(deadline, wait)) {
                abort = Abort::timed_out;
                break;
            }
            wait_ms = static_cast<int>(wait.count());
        }
        pollfd pfd{pipe_fds[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            abort = Abort::failed;
            break;
        }
        if (ready == 0) continue; // slice elapsed
        const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.output.append(buffer, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break; // EOF: every writer (the child and its children) closed
        } else if (errno != EINTR && errno != EAGAIN) {
            abort = Abort::failed;
            break;
        }
    }
    // cancel() kills the child, which ends the loop at EOF: still a cancel.
    if (abort == Abort::none && options.canceller != nullptr && options.canceller->cancelled()) {
        abort = Abort::cancelled;
    }
    if (abort != Abort::none) kill_child(pid);
    ::close(pipe_fds[0]);
    detail::CancellerAccess::detach(options.canceller); // before reaping: see kill_child

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return fail(std::string("waitpid failed: ") + std::strerror(errno));
        }
    }
    if (abort != Abort::none) return abort_error(abort, program, options, result.output);
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
    // UTF-8 (read from the UTF-16 environment on Windows); each entry is
    // converted back with path_from_utf8.
    const std::optional<std::string> path_env = env_utf8("PATH");
    if (!path_env) return std::nullopt;
#if defined(_WIN32)
    constexpr char separator = ';';
    const std::string file = std::string(name) + ".exe";
#else
    constexpr char separator = ':';
    const std::string file(name);
#endif
    const std::string_view paths(*path_env);
    std::size_t start = 0;
    while (start <= paths.size()) {
        std::size_t end = paths.find(separator, start);
        if (end == std::string_view::npos) end = paths.size();
        const std::string_view dir = paths.substr(start, end - start);
        if (!dir.empty()) {
            const fs::path candidate = path_from_utf8(dir) / file;
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
