#pragma once

// Runs a program as a child process and captures its output (ROADMAP R2: the
// shader compiler service runs slangc this way). No shell is involved: the
// arguments are passed to the program verbatim.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::core {

struct ProcessResult {
    int         exit_code = -1;  // valid when !signaled
    bool        signaled  = false; // POSIX: terminated by a signal (e.g. a crash)
    int         signal    = 0;
    std::string output;          // stdout and stderr, interleaved as written

    [[nodiscard]] bool succeeded() const { return !signaled && exit_code == 0; }
};

namespace detail {
struct CancellerAccess;
} // namespace detail

// Lets one thread stop the child process that run_process() is waiting on in
// another thread (e.g. a compile worker being shut down). Thread-safe.
class ProcessCanceller {
public:
    ProcessCanceller() = default;
    ProcessCanceller(const ProcessCanceller&) = delete;
    ProcessCanceller& operator=(const ProcessCanceller&) = delete;

    // Kills the running child (POSIX: its whole process group), if any, and
    // makes every later run_process() using this canceller fail at once.
    // Idempotent.
    void cancel();
    [[nodiscard]] bool cancelled() const;

private:
    friend struct detail::CancellerAccess;
    mutable std::mutex mutex_;
    bool               cancelled_ = false;
    // The registered child while run_process() waits on it: a pid (POSIX) or
    // a process HANDLE (Windows). Cleared before the child is reaped, so
    // cancel() never signals a recycled pid.
    std::intptr_t      child_ = 0;
    bool               has_child_ = false;
};

struct ProcessOptions {
    // Kill the child and fail when it has not exited after this long
    // (zero: wait indefinitely).
    std::chrono::milliseconds timeout{0};
    ProcessCanceller*         canceller = nullptr; // optional
};

// Runs `program` with `args` (argv[1..]), blocking until it exits. Fails when
// the process cannot be started, exceeds `options.timeout` or is cancelled (the
// child is killed in both cases and the error carries its output so far); a
// non-zero exit is a ProcessResult. The child's stdin is the null device.
//
// Thread safety: concurrent calls from any threads are safe — no call's
// output pipe leaks into another call's child (Linux/BSD: pipe2(O_CLOEXEC);
// Windows: the child inherits only its own handles via
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST; other POSIX systems: pipe creation and
// spawn are serialised). Not covered: code elsewhere in the process that
// spawns with blanket inheritance — Windows CreateProcess(bInheritHandles=TRUE)
// without a handle list, or on POSIX systems without pipe2 a raw fork/exec —
// can still inherit the pipe while a spawn is in progress.
[[nodiscard]] std::expected<ProcessResult, Error>
run_process(const std::filesystem::path& program, std::span<const std::string> args,
            const ProcessOptions& options = {});

// Finds an executable named `name` on PATH (".exe" is appended on Windows).
[[nodiscard]] std::optional<std::filesystem::path> find_on_path(std::string_view name);

} // namespace engine::core
