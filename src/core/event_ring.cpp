// Engine event ring — see include/engine/core/event_ring.hpp.

#include "engine/core/event_ring.hpp"

#include <algorithm>

namespace engine::core {

std::string_view to_string(EventKind kind) {
    switch (kind) {
    case EventKind::contact_added:
        return "contact_added";
    case EventKind::contact_removed:
        return "contact_removed";
    case EventKind::trigger_enter:
        return "trigger_enter";
    case EventKind::trigger_exit:
        return "trigger_exit";
    }
    return "unknown";
}

EventRing::EventRing(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

std::uint64_t EventRing::push(Event event) {
    const std::scoped_lock lock(mutex_);
    event.seq = next_seq_++;
    if (events_.size() == capacity_) events_.pop_front();
    events_.push_back(event);
    return event.seq;
}

void EventRing::drop(std::uint64_t count) {
    const std::scoped_lock lock(mutex_);
    next_seq_ += count;
}

EventQueryResult EventRing::query(std::uint64_t since, std::size_t max) const {
    const std::scoped_lock lock(mutex_);
    EventQueryResult out;
    out.last_seq = next_seq_ - 1;
    out.oldest_seq = events_.empty() ? 0 : events_.front().seq;
    if (since > out.last_seq) since = 0; // a cursor from a previous engine run
    const auto first = std::upper_bound(
        events_.begin(), events_.end(), since,
        [](std::uint64_t s, const Event& e) { return s < e.seq; });
    for (auto it = first; it != events_.end() && out.events.size() < max; ++it) {
        out.events.push_back(*it);
    }
    const std::size_t available = static_cast<std::size_t>(events_.end() - first);
    out.more = available > out.events.size();
    if (!out.more) {
        out.cursor = out.last_seq;
    } else {
        out.cursor = out.events.empty() ? since : out.events.back().seq; // max == 0: nothing read
    }
    out.dropped = out.cursor - since - out.events.size();
    return out;
}

std::uint64_t EventRing::last_seq() const {
    const std::scoped_lock lock(mutex_);
    return next_seq_ - 1;
}

std::size_t EventRing::size() const {
    const std::scoped_lock lock(mutex_);
    return events_.size();
}

EventRing& event_ring() {
    static EventRing ring;
    return ring;
}

} // namespace engine::core
