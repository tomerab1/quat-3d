#pragma once

// Engine event ring (ROADMAP A10.1).
//
// EventRing is a fixed-capacity buffer of the most recent gameplay events,
// each tagged with a monotonically increasing sequence number so a reader
// (the remote-control `events.poll` method) can tail it incrementally:
// "everything after seq N". When full, the oldest event is overwritten.
// Events lost before they were recorded (a producer's own buffer overflowed)
// still consume sequence numbers through drop(), so a reader learns about
// every gap: `dropped` counts the sequence numbers after its cursor that it
// can no longer read, whatever the reason.
//
// Physics contacts and trigger overlaps are the first producers: the physics
// world collects them lock-free on its worker threads and the frame loop
// pushes them here after each simulation update (main thread). The ring
// itself is thread-safe.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::core {

enum class EventKind : std::uint8_t { contact_added, contact_removed, trigger_enter, trigger_exit };

[[nodiscard]] std::string_view to_string(EventKind kind);

struct Event {
    static constexpr std::uint32_t no_entity = 0xFFFFFFFFU;

    std::uint64_t        seq = 0; // assigned by push(); 1-based, increasing, never reused
    EventKind            kind = EventKind::contact_added;
    double               time_s = 0.0; // simulated time of the producing world, seconds
    std::uint32_t        entity_a = no_entity; // trigger events: the trigger volume
    std::uint32_t        entity_b = no_entity;
    std::array<float, 3> point{};  // world space (contact_added, trigger_enter)
    std::array<float, 3> normal{}; // from a towards b (contact_added)
    float                impulse = 0.0F; // estimated impact impulse, N*s (contact_added)
};

struct EventQueryResult {
    std::vector<Event> events; // oldest first
    // Pass as the next `since`: every sequence number up to it was examined.
    std::uint64_t cursor = 0;
    // Sequence numbers in (since, cursor] that could not be returned: they
    // were overwritten before this read or never recorded (producer overflow).
    std::uint64_t dropped = 0;
    // `max` was reached before the newest event; call again from cursor.
    bool          more = false;
    std::uint64_t oldest_seq = 0; // oldest retained event (0 when empty)
    std::uint64_t last_seq = 0;   // newest sequence number handed out
};

class EventRing {
public:
    static constexpr std::size_t default_capacity = 8192;

    explicit EventRing(std::size_t capacity = default_capacity);
    EventRing(const EventRing&) = delete;
    EventRing& operator=(const EventRing&) = delete;

    // Records `event` (its seq is assigned here) and returns the sequence number.
    std::uint64_t push(Event event);
    // `count` events were lost before they could be recorded: their sequence
    // numbers are consumed so readers see the gap.
    void drop(std::uint64_t count);

    // Events with seq > since, oldest first, at most `max`. A `since` beyond
    // the newest sequence number (a cursor from before an engine restart)
    // reads from the oldest retained event.
    [[nodiscard]] EventQueryResult query(std::uint64_t since, std::size_t max) const;

    [[nodiscard]] std::uint64_t last_seq() const;
    [[nodiscard]] std::size_t   size() const;
    [[nodiscard]] std::size_t   capacity() const { return capacity_; }

private:
    const std::size_t  capacity_;
    mutable std::mutex mutex_;
    std::deque<Event>  events_;
    std::uint64_t      next_seq_ = 1;
};

// The process-wide ring (created on first use, thread-safe).
[[nodiscard]] EventRing& event_ring();

} // namespace engine::core
