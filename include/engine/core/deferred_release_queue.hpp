#pragma once

// DeferredReleaseQueue — destroys GPU objects only once no frame in flight can
// still reference them (ROADMAP R2: pipelines replaced by shader hot reload).
//
// Frame numbering: the caller passes a monotonically increasing frame number,
// advanced once per submitted frame, and calls collect() after waiting for the
// in-flight fence of the frame about to be recorded. An object pushed at frame
// N may be referenced by frames up to N; with F frames in flight, frame N is
// known complete when frame N + F begins, so collect(N + F) releases it.
// Releases run in push order.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace engine::core {

class DeferredReleaseQueue {
public:
    explicit DeferredReleaseQueue(std::uint32_t frames_in_flight = 2)
        : frames_in_flight_(frames_in_flight) {}
    ~DeferredReleaseQueue() { flush(); }

    DeferredReleaseQueue(DeferredReleaseQueue&& other) noexcept = default;
    DeferredReleaseQueue& operator=(DeferredReleaseQueue&& other) noexcept {
        if (this != &other) {
            flush();
            frames_in_flight_ = other.frames_in_flight_;
            entries_ = std::move(other.entries_);
            other.entries_.clear();
        }
        return *this;
    }
    DeferredReleaseQueue(const DeferredReleaseQueue&) = delete;
    DeferredReleaseQueue& operator=(const DeferredReleaseQueue&) = delete;

    // Schedules `release` for an object last usable by frame `frame`.
    void push(std::uint64_t frame, std::function<void()> release) {
        entries_.push_back(Entry{frame, std::move(release)});
    }

    // Runs every release whose frame + frames_in_flight <= current_frame (in
    // push order) and returns how many ran.
    std::size_t collect(std::uint64_t current_frame) {
        std::size_t released = 0;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].frame + frames_in_flight_ <= current_frame) {
                if (entries_[i].release) entries_[i].release();
                ++released;
            } else {
                if (kept != i) entries_[kept] = std::move(entries_[i]);
                ++kept;
            }
        }
        entries_.resize(kept);
        return released;
    }

    // Runs every pending release now (the device must be idle).
    void flush() {
        for (Entry& e : entries_) {
            if (e.release) e.release();
        }
        entries_.clear();
    }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] std::uint32_t frames_in_flight() const { return frames_in_flight_; }
    void set_frames_in_flight(std::uint32_t frames) { frames_in_flight_ = frames; }

private:
    struct Entry {
        std::uint64_t         frame = 0;
        std::function<void()> release;
    };

    std::uint32_t      frames_in_flight_ = 2;
    std::vector<Entry> entries_;
};

} // namespace engine::core
