// StderrCapture — see engine/core/stderr_capture.hpp.

#include "engine/core/stderr_capture.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "engine/core/log.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define QUAT_STDERR_CAPTURE_POSIX 1
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace engine::core {

namespace {

// The original stderr while a capture is active, else -1. Guarded by
// g_fd_mutex for writes and for the swap in start()/stop().
std::mutex       g_fd_mutex;
int              g_real_fd = -1;
std::atomic_bool g_active{false};

#ifdef QUAT_STDERR_CAPTURE_POSIX
// Writes all of `data` to `fd`, retrying on EINTR / short writes. Gives up on
// any other error (a closed terminal must not stall the engine).
void write_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n > 0) {
            data += n;
            size -= static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

[[nodiscard]] bool set_flags(int fd, bool nonblocking) {
    const int fd_flags = ::fcntl(fd, F_GETFD);
    if (fd_flags < 0 || ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) return false;
    if (!nonblocking) return true;
    const int fl = ::fcntl(fd, F_GETFL);
    return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) >= 0;
}
#endif

} // namespace

void write_real_stderr(std::string_view text) {
#ifdef QUAT_STDERR_CAPTURE_POSIX
    const std::scoped_lock lock(g_fd_mutex);
    write_all(g_real_fd >= 0 ? g_real_fd : STDERR_FILENO, text.data(), text.size());
#else
    std::fwrite(text.data(), 1, text.size(), stderr);
#endif
}

#ifdef QUAT_STDERR_CAPTURE_POSIX

struct StderrCapture::Impl {
    LogRing*    ring = nullptr;
    int         read_fd = -1;  // pipe the process's fd 2 now writes into
    int         wake_read = -1;
    int         wake_write = -1;
    int         saved_fd = -1; // the original stderr
    std::thread thread;
    std::atomic_bool stopping{false};

    std::mutex  drain_mutex; // one reader at a time (thread or flush())
    std::string partial;     // bytes after the last newline

    static constexpr std::size_t k_max_line = LogRing::max_message_bytes;

    void push_line(std::string_view line) {
        if (line.empty()) return;
        ParsedLine p = parse_stderr_line(line);
        if (p.message.empty() && p.category == "stderr") return;
        ring->push(p.level, p.category, p.message);
    }

    void consume(const char* data, std::size_t size) {
        {
            const std::scoped_lock lock(g_fd_mutex);
            write_all(saved_fd, data, size);
        }
        partial.append(data, size);
        std::size_t start = 0;
        for (std::size_t nl; (nl = partial.find('\n', start)) != std::string::npos; start = nl + 1) {
            push_line(std::string_view(partial).substr(start, nl - start));
        }
        partial.erase(0, start);
        // A runaway line without newlines is recorded in pieces.
        while (partial.size() >= k_max_line) {
            push_line(std::string_view(partial).substr(0, k_max_line));
            partial.erase(0, k_max_line);
        }
    }

    // Reads everything currently in the pipe (non-blocking).
    void drain() {
        const std::scoped_lock lock(drain_mutex);
        char buf[16384];
        for (;;) {
            const ssize_t n = ::read(read_fd, buf, sizeof(buf));
            if (n > 0) {
                consume(buf, static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                return; // EAGAIN (empty), EOF or error
            }
        }
    }

    void run() {
        pollfd fds[2]{{read_fd, POLLIN, 0}, {wake_read, POLLIN, 0}};
        while (!stopping.load(std::memory_order_acquire)) {
            fds[0].revents = 0;
            fds[1].revents = 0;
            const int r = ::poll(fds, 2, -1);
            if (r < 0 && errno != EINTR) break;
            if (fds[0].revents != 0) drain();
            if ((fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 &&
                (fds[0].revents & POLLIN) == 0) {
                break; // every writer is gone
            }
        }
    }

    void close_all() {
        for (int* fd : {&read_fd, &wake_read, &wake_write, &saved_fd}) {
            if (*fd >= 0) ::close(*fd);
            *fd = -1;
        }
    }
};

std::expected<StderrCapture, Error> StderrCapture::start(LogRing& ring) {
    if (g_active.exchange(true)) return std::unexpected(Error{"stderr is already being captured"});
    auto impl = std::make_unique<Impl>();
    impl->ring = &ring;
    const auto fail = [&](const char* what) {
        const int err = errno;
        impl->close_all();
        g_active.store(false);
        return std::unexpected(Error{std::string(what) + ": " + std::to_string(err)});
    };

    int data_pipe[2] = {-1, -1};
    if (::pipe(data_pipe) != 0) return fail("pipe() failed, errno");
    impl->read_fd = data_pipe[0];
    const int write_fd = data_pipe[1];
    int wake_pipe[2] = {-1, -1};
    if (::pipe(wake_pipe) != 0) {
        ::close(write_fd);
        return fail("pipe() failed, errno");
    }
    impl->wake_read = wake_pipe[0];
    impl->wake_write = wake_pipe[1];
    if (!set_flags(impl->read_fd, true) || !set_flags(impl->wake_read, true) ||
        !set_flags(impl->wake_write, true) || !set_flags(write_fd, false)) {
        ::close(write_fd);
        return fail("fcntl() failed, errno");
    }
#ifdef F_SETPIPE_SZ
    // Headroom for bursts while the drain thread is descheduled (best effort).
    (void)::fcntl(write_fd, F_SETPIPE_SZ, 1 << 20);
#endif

    std::fflush(stderr);
    {
        const std::scoped_lock lock(g_fd_mutex);
        impl->saved_fd = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
        if (impl->saved_fd < 0 || ::dup2(write_fd, STDERR_FILENO) < 0) {
            ::close(write_fd);
            return fail("redirecting stderr failed, errno");
        }
        g_real_fd = impl->saved_fd;
    }
    ::close(write_fd); // fd 2 now holds the pipe's only write end

    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    StderrCapture capture;
    capture.impl_ = std::move(impl);
    return capture;
}

void StderrCapture::flush() {
    if (!impl_) return;
    std::fflush(stderr);
    impl_->drain();
}

void StderrCapture::stop() {
    if (!impl_) return;
    std::fflush(stderr);
    {
        // Point fd 2 back at the original: new writes bypass the pipe.
        const std::scoped_lock lock(g_fd_mutex);
        (void)::dup2(impl_->saved_fd, STDERR_FILENO);
        g_real_fd = -1;
    }
    impl_->stopping.store(true, std::memory_order_release);
    const char byte = 1;
    (void)::write(impl_->wake_write, &byte, 1);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->drain(); // anything written just before the switch
    if (!impl_->partial.empty()) {
        impl_->push_line(impl_->partial);
        impl_->partial.clear();
    }
    impl_->close_all();
    impl_.reset();
    g_active.store(false);
}

#else // !QUAT_STDERR_CAPTURE_POSIX

struct StderrCapture::Impl {};

std::expected<StderrCapture, Error> StderrCapture::start(LogRing&) {
    return std::unexpected(Error{"stderr capture is not supported on this platform"});
}
void StderrCapture::flush() {}
void StderrCapture::stop() { impl_.reset(); }

#endif

StderrCapture::StderrCapture() = default;
StderrCapture::StderrCapture(StderrCapture&&) noexcept = default;
StderrCapture& StderrCapture::operator=(StderrCapture&& other) noexcept {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
StderrCapture::~StderrCapture() { stop(); }

} // namespace engine::core
