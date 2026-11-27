// LogRing, the process-wide ring and the log() helper — see engine/core/log.hpp.

#include "engine/core/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "engine/core/stderr_capture.hpp"

namespace engine::core {

namespace {

[[nodiscard]] char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// Cuts `s` to at most `max` bytes without splitting a UTF-8 sequence.
[[nodiscard]] std::string_view clip_utf8(std::string_view s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0U) == 0x80U) --n;
    return s.substr(0, n);
}

[[nodiscard]] bool matches(const LogLine& line, const LogQuery& q) {
    return line.level >= q.min_level && (q.category.empty() || line.category == q.category) &&
           (q.contains.empty() || line.message.find(q.contains) != std::string::npos);
}

} // namespace

std::string_view to_string(LogLevel level) {
    switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warn: return "warn";
    case LogLevel::error: return "error";
    }
    return "info";
}

std::optional<LogLevel> parse_log_level(std::string_view name) {
    struct Alias {
        std::string_view name;
        LogLevel         level;
    };
    static constexpr std::array<Alias, 8> aliases{{{"trace", LogLevel::trace},
                                                   {"debug", LogLevel::debug},
                                                   {"info", LogLevel::info},
                                                   {"warn", LogLevel::warn},
                                                   {"warning", LogLevel::warn},
                                                   {"error", LogLevel::error},
                                                   {"err", LogLevel::error},
                                                   {"fatal", LogLevel::error}}};
    for (const Alias& a : aliases) {
        if (iequals(name, a.name)) return a.level;
    }
    return std::nullopt;
}

// ---- LogRing -------------------------------------------------------------------

LogRing::LogRing(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)), start_(std::chrono::steady_clock::now()) {
    lines_.reserve(capacity_);
}

std::uint64_t LogRing::push(LogLevel level, std::string_view category, std::string_view message) {
    LogLine line;
    line.level = level;
    line.category = std::string(clip_utf8(category, 64));
    line.message = std::string(clip_utf8(message, max_message_bytes));
    line.time_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();

    const std::scoped_lock lock(mutex_);
    line.seq = next_seq_++;
    const std::uint64_t seq = line.seq;
    if (lines_.size() < capacity_) {
        lines_.push_back(std::move(line));
    } else {
        lines_[head_] = std::move(line);
        head_ = (head_ + 1) % capacity_;
    }
    return seq;
}

LogQueryResult LogRing::query(const LogQuery& q) const {
    LogQueryResult out;
    const std::size_t limit = std::max<std::size_t>(q.limit, 1);
    const std::scoped_lock lock(mutex_);
    const std::size_t n = lines_.size();
    const auto at = [&](std::size_t i) -> const LogLine& { return lines_[(head_ + i) % n]; };
    const std::uint64_t last = next_seq_ - 1;
    out.cursor = last;
    if (n == 0) return out;
    const std::uint64_t first = at(0).seq;

    if (!q.since_seq) { // newest `limit` matches, returned oldest first
        for (std::size_t i = n; i-- > 0;) {
            if (!matches(at(i), q)) continue;
            if (out.lines.size() == limit) {
                out.more = true;
                break;
            }
            out.lines.push_back(at(i));
        }
        std::reverse(out.lines.begin(), out.lines.end());
        return out;
    }

    // A cursor beyond the newest line comes from an earlier process (the
    // engine restarted): read from the oldest retained line.
    const std::uint64_t since = *q.since_seq > last ? 0 : *q.since_seq;
    if (since + 1 < first) out.missed = first - since - 1;
    // Index of the first line with seq > since (seqs are contiguous in the ring).
    const std::size_t begin = since + 1 <= first ? 0 : static_cast<std::size_t>(since + 1 - first);
    for (std::size_t i = begin; i < n; ++i) {
        const LogLine& line = at(i);
        if (!matches(line, q)) continue;
        if (out.lines.size() == limit) {
            out.more = true;
            out.cursor = out.lines.back().seq;
            return out;
        }
        out.lines.push_back(line);
    }
    return out;
}

std::uint64_t LogRing::last_seq() const {
    const std::scoped_lock lock(mutex_);
    return next_seq_ - 1;
}

std::uint64_t LogRing::first_seq() const {
    const std::scoped_lock lock(mutex_);
    return lines_.empty() ? 0 : lines_[lines_.size() < capacity_ ? 0 : head_].seq;
}

std::size_t LogRing::size() const {
    const std::scoped_lock lock(mutex_);
    return lines_.size();
}

LogRing& log_ring() {
    static LogRing ring;
    return ring;
}

// ---- stderr line parsing -----------------------------------------------------------

ParsedLine parse_stderr_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    ParsedLine out;
    std::string_view rest = line;
    bool have_category = false;
    // At most two leading tags: "[vulkan][ERROR] ...".
    for (int tags = 0; tags < 2 && rest.size() > 2 && rest.front() == '['; ++tags) {
        const std::size_t close = rest.find(']');
        if (close == std::string_view::npos || close == 1 || close > 32) break;
        const std::string_view tag = rest.substr(1, close - 1);
        if (tag.find_first_of(" \t") != std::string_view::npos) break;
        if (const auto level = parse_log_level(tag)) {
            out.level = *level;
        } else if (!have_category) {
            out.category = std::string(tag);
            have_category = true;
        } else {
            break;
        }
        rest.remove_prefix(close + 1);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    }
    if (!have_category) out.category = "stderr";
    out.message = std::string(rest);
    return out;
}

// ---- log() ---------------------------------------------------------------------------

void log_message(LogLevel level, std::string_view category, std::string_view message) {
    log_ring().push(level, category, message);
    std::string text;
    text.reserve(category.size() + message.size() + 16);
    text += '[';
    text += category;
    text += ']';
    if (level != LogLevel::info) {
        text += '[';
        text += to_string(level);
        text += ']';
    }
    text += ' ';
    text += message;
    text += '\n';
    write_real_stderr(text);
}

} // namespace engine::core
