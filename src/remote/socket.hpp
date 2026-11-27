#pragma once

// Minimal non-blocking loopback TCP sockets for the remote-control server
// (private to engine_remote). POSIX sockets on Linux/macOS, Winsock on
// Windows. Only what RpcServer needs: listen/accept on 127.0.0.1, connect (for
// the self-test client), and non-blocking send/recv.

#include <cstddef>
#include <cstdint>
#include <expected>

#include "engine/core/error.hpp"

namespace engine::remote::detail {

#if defined(_WIN32)
using NativeSocket = std::uintptr_t; // SOCKET
inline constexpr NativeSocket invalid_native_socket = ~static_cast<std::uintptr_t>(0);
#else
using NativeSocket = int;
inline constexpr NativeSocket invalid_native_socket = -1;
#endif

// Owning, move-only socket handle.
class Socket {
public:
    Socket() = default;
    explicit Socket(NativeSocket s) : s_(s) {}
    Socket(Socket&& o) noexcept : s_(o.s_) { o.s_ = invalid_native_socket; }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            s_ = o.s_;
            o.s_ = invalid_native_socket;
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    [[nodiscard]] bool valid() const { return s_ != invalid_native_socket; }
    [[nodiscard]] NativeSocket native() const { return s_; }
    void close();

private:
    NativeSocket s_ = invalid_native_socket;
};

enum class IoStatus : std::uint8_t { ok, would_block, closed, error };
struct IoResult {
    IoStatus    status = IoStatus::error;
    std::size_t bytes = 0;
};

// Non-blocking listener on 127.0.0.1:<port> (0 = ephemeral).
[[nodiscard]] std::expected<Socket, core::Error> listen_loopback(std::uint16_t port);
[[nodiscard]] std::uint16_t local_port(const Socket& s);
// Next pending connection (non-blocking, TCP_NODELAY), or unexpected when none.
[[nodiscard]] std::expected<Socket, core::Error> accept_client(const Socket& listener);
// Blocking connect to 127.0.0.1:<port>, then switched to non-blocking.
[[nodiscard]] std::expected<Socket, core::Error> connect_loopback(std::uint16_t port);

[[nodiscard]] IoResult recv_some(const Socket& s, char* buf, std::size_t len);
[[nodiscard]] IoResult send_some(const Socket& s, const char* buf, std::size_t len);

} // namespace engine::remote::detail
