#include "socket.hpp"

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace engine::remote::detail {

namespace {

#if defined(_WIN32)
// Winsock needs process-wide initialisation; done once, never torn down
// (WSACleanup at exit is optional and racy with other users).
[[nodiscard]] bool ensure_winsock() {
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
[[nodiscard]] bool last_would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
[[nodiscard]] std::string last_error_text() { return "WSA error " + std::to_string(WSAGetLastError()); }
[[nodiscard]] bool set_non_blocking(NativeSocket s) {
    u_long on = 1;
    return ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &on) == 0;
}
#else
[[nodiscard]] bool ensure_winsock() { return true; }
[[nodiscard]] bool last_would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
[[nodiscard]] std::string last_error_text() { return "errno " + std::to_string(errno); }
[[nodiscard]] bool set_non_blocking(NativeSocket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

[[nodiscard]] sockaddr_in loopback_addr(std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

void tune_stream(NativeSocket s) {
    int one = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof(one));
#if defined(__APPLE__)
    // macOS has no MSG_NOSIGNAL: suppress SIGPIPE per socket instead.
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

[[nodiscard]] std::unexpected<core::Error> sock_fail(const std::string& what) {
    return std::unexpected(core::Error{"remote socket: " + what + " (" + last_error_text() + ")"});
}

} // namespace

void Socket::close() {
    if (s_ == invalid_native_socket) return;
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(s_));
#else
    ::close(s_);
#endif
    s_ = invalid_native_socket;
}

std::expected<Socket, core::Error> listen_loopback(std::uint16_t port) {
    if (!ensure_winsock()) return sock_fail("WSAStartup failed");
    Socket s(static_cast<NativeSocket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!s.valid()) return sock_fail("socket()");
#if !defined(_WIN32)
    // Allow quick restarts while old connections sit in TIME_WAIT. (On
    // Windows SO_REUSEADDR would let another process steal the port.)
    int one = 1;
    (void)setsockopt(s.native(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    const sockaddr_in addr = loopback_addr(port);
    if (bind(s.native(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return sock_fail("bind 127.0.0.1:" + std::to_string(port));
    }
    if (::listen(s.native(), 8) != 0) return sock_fail("listen");
    if (!set_non_blocking(s.native())) return sock_fail("non-blocking listener");
    return s;
}

std::uint16_t local_port(const Socket& s) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(s.native(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

std::expected<Socket, core::Error> accept_client(const Socket& listener) {
    const auto raw = accept(listener.native(), nullptr, nullptr);
    Socket s(static_cast<NativeSocket>(raw));
    if (!s.valid()) return std::unexpected(core::Error{"no pending connection"});
    if (!set_non_blocking(s.native())) return sock_fail("non-blocking client");
    tune_stream(s.native());
    return s;
}

std::expected<Socket, core::Error> connect_loopback(std::uint16_t port) {
    if (!ensure_winsock()) return sock_fail("WSAStartup failed");
    Socket s(static_cast<NativeSocket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!s.valid()) return sock_fail("socket()");
    const sockaddr_in addr = loopback_addr(port);
    if (connect(s.native(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return sock_fail("connect 127.0.0.1:" + std::to_string(port));
    }
    if (!set_non_blocking(s.native())) return sock_fail("non-blocking connect");
    tune_stream(s.native());
    return s;
}

IoResult recv_some(const Socket& s, char* buf, std::size_t len) {
#if defined(_WIN32)
    const int n = recv(static_cast<SOCKET>(s.native()), buf, static_cast<int>(len), 0);
#else
    const ssize_t n = recv(s.native(), buf, len, 0);
#endif
    if (n > 0) return {IoStatus::ok, static_cast<std::size_t>(n)};
    if (n == 0) return {IoStatus::closed, 0};
    return {last_would_block() ? IoStatus::would_block : IoStatus::error, 0};
}

IoResult send_some(const Socket& s, const char* buf, std::size_t len) {
#if defined(_WIN32)
    const int n = send(static_cast<SOCKET>(s.native()), buf, static_cast<int>(len), 0);
#elif defined(__linux__)
    const ssize_t n = send(s.native(), buf, len, MSG_NOSIGNAL);
#else
    const ssize_t n = send(s.native(), buf, len, 0);
#endif
    if (n >= 0) return {IoStatus::ok, static_cast<std::size_t>(n)};
    return {last_would_block() ? IoStatus::would_block : IoStatus::error, 0};
}

} // namespace engine::remote::detail
