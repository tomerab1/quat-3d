#pragma once

// Rolling statistics for the built-in frame profiler (ROADMAP R4.2): a
// fixed-capacity ring of float samples (the last N frames of one zone) and the
// summary every profiler view reports — average, median, 95th percentile and
// maximum.
//
// Percentiles use the nearest-rank definition (the smallest sample with at
// least p% of the samples at or below it), the usual choice for frame-time
// reports: p95 of 240 frames is the 228th fastest frame, always a real sample.

#include <cstdint>
#include <span>
#include <vector>

namespace engine::core {

struct SampleStats {
    std::uint32_t count = 0; // samples summarised (0: every field is 0)
    float         avg = 0.0F;
    float         p50 = 0.0F;
    float         p95 = 0.0F;
    float         max = 0.0F;
    float         last = 0.0F; // most recent sample
};

// Nearest-rank percentile `p` (clamped to [0, 100]) of `values`, which it
// reorders. 0 for an empty span; p = 0 gives the minimum.
[[nodiscard]] float percentile(std::span<float> values, float p);

// avg / p50 / p95 / max of `values` (reordered); `last` is left 0 — the
// caller knows which sample was the latest.
[[nodiscard]] SampleStats compute_stats(std::span<float> values);

// The last `capacity` samples pushed, oldest overwritten first. Storage is
// allocated once by the constructor; push() never allocates.
class SampleRing {
public:
    SampleRing() = default;
    explicit SampleRing(std::uint32_t capacity) : data_(capacity, 0.0F) {}

    void push(float value) noexcept {
        if (data_.empty()) return;
        data_[head_] = value;
        head_ = (head_ + 1) % static_cast<std::uint32_t>(data_.size());
        if (size_ < data_.size()) ++size_;
    }
    void clear() noexcept {
        head_ = 0;
        size_ = 0;
    }

    [[nodiscard]] std::uint32_t size() const { return size_; }
    [[nodiscard]] std::uint32_t capacity() const { return static_cast<std::uint32_t>(data_.size()); }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    // Sample `i` in chronological order (0 = oldest kept). Requires i < size().
    [[nodiscard]] float at(std::uint32_t i) const;
    // The newest sample, 0 when empty.
    [[nodiscard]] float latest() const { return size_ == 0 ? 0.0F : at(size_ - 1); }
    // Appends the samples to `out`, oldest first.
    void copy_to(std::vector<float>& out) const;
    // Statistics over the kept samples (`last` = latest()). Allocates a
    // scratch copy: a query, not a per-frame hot path.
    [[nodiscard]] SampleStats stats() const;

private:
    std::vector<float> data_;
    std::uint32_t      head_ = 0; // next write
    std::uint32_t      size_ = 0;
};

} // namespace engine::core
