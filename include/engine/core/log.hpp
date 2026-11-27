#pragma once

// Engine log ring (ROADMAP A5; the logging layer proper is S6).
//
// LogRing is a fixed-capacity, thread-safe in-memory buffer of the most recent
// log lines, each tagged with a monotonically increasing sequence number so a
// reader (the remote-control `debug.log` method, later the editor console) can
// tail it incrementally: "everything after seq N". When full, the oldest line
// is overwritten; readers learn how many lines they missed.
//
// Lines reach the process-wide ring two ways:
//   - engine::core::log(level, category, fmt, args...) — structured: pushed
//     into the ring and echoed to the real stderr as "[category] message"
//     (warnings/errors as "[category][warn] message").
//   - StderrCapture (stderr_capture.hpp) — everything else the process writes
//     to stderr (today's fprintf(stderr, ...) calls, validation layers,
//     third-party libraries), parsed by parse_stderr_line().

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::core {

enum class LogLevel : std::uint8_t { trace, debug, info, warn, error };

[[nodiscard]] std::string_view to_string(LogLevel level);
// Accepts trace|debug|info|warn|warning|error|err|fatal, case-insensitive.
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view name);

struct LogLine {
    std::uint64_t seq = 0;      // 1-based, increasing, never reused
    double        time_s = 0.0; // seconds since the ring was created
    LogLevel      level = LogLevel::info;
    std::string   category;
    std::string   message;
};

struct LogQuery {
    // Return only lines with seq > since_seq, oldest first (incremental
    // tail); a since_seq beyond the newest line (a cursor from before an
    // engine restart) reads from the oldest retained line. nullopt: the
    // newest `limit` matching lines, oldest first (a snapshot).
    std::optional<std::uint64_t> since_seq;
    LogLevel                     min_level = LogLevel::trace;
    std::string                  category; // exact match; empty = any
    std::string                  contains; // case-sensitive substring of message; empty = any
    std::size_t                  limit = 100;
};

struct LogQueryResult {
    std::vector<LogLine> lines;
    // Pass as the next since_seq: every line up to it has been examined.
    std::uint64_t cursor = 0;
    // Lines after since_seq that were overwritten before this read.
    std::uint64_t missed = 0;
    // `limit` was reached before the end of the ring; call again from cursor.
    bool more = false;
};

class LogRing {
public:
    static constexpr std::size_t default_capacity = 4096;
    static constexpr std::size_t max_message_bytes = 16 * 1024; // longer messages are cut

    explicit LogRing(std::size_t capacity = default_capacity);
    LogRing(const LogRing&) = delete;
    LogRing& operator=(const LogRing&) = delete;

    // Thread-safe. Returns the new line's sequence number.
    std::uint64_t push(LogLevel level, std::string_view category, std::string_view message);

    // Thread-safe snapshot query (see LogQuery).
    [[nodiscard]] LogQueryResult query(const LogQuery& q) const;

    [[nodiscard]] std::uint64_t last_seq() const;  // 0 when nothing was pushed
    [[nodiscard]] std::uint64_t first_seq() const; // oldest retained (0 when empty)
    [[nodiscard]] std::size_t   size() const;
    [[nodiscard]] std::size_t   capacity() const { return capacity_; }

private:
    const std::size_t                     capacity_;
    const std::chrono::steady_clock::time_point start_;
    mutable std::mutex                    mutex_;
    std::vector<LogLine>                  lines_; // circular once full
    std::size_t                           head_ = 0; // index of the oldest line when full
    std::uint64_t                         next_seq_ = 1;
};

// The process-wide ring (created on first use, thread-safe).
[[nodiscard]] LogRing& log_ring();

// A captured stderr line split into level / category / message. Leading
// bracket tags are interpreted: "[warn] x" -> (warn, "stderr", "x");
// "[nav] x" -> (info, "nav", "x"); "[vulkan][ERROR] x" -> (error, "vulkan",
// "x"). Untagged lines are (info, "stderr", line).
struct ParsedLine {
    LogLevel    level = LogLevel::info;
    std::string category;
    std::string message;
};
[[nodiscard]] ParsedLine parse_stderr_line(std::string_view line);

// Pushes into log_ring() and echoes to the real stderr (bypassing an active
// StderrCapture, so the line is not recorded twice).
void log_message(LogLevel level, std::string_view category, std::string_view message);

template <typename... Args>
void log(LogLevel level, std::string_view category, std::format_string<Args...> fmt,
         Args&&... args) {
    log_message(level, category, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace engine::core
