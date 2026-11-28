#include "engine/core/profile_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::core {

float percentile(std::span<float> values, float p) {
    if (values.empty()) return 0.0F;
    const float clamped = std::clamp(p, 0.0F, 100.0F);
    // Nearest rank: ceil(p / 100 * n), at least 1.
    const auto n = static_cast<double>(values.size());
    auto rank = static_cast<std::size_t>(std::ceil(static_cast<double>(clamped) / 100.0 * n));
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    const auto nth = values.begin() + static_cast<std::ptrdiff_t>(rank - 1);
    std::nth_element(values.begin(), nth, values.end());
    return *nth;
}

SampleStats compute_stats(std::span<float> values) {
    SampleStats s;
    if (values.empty()) return s;
    s.count = static_cast<std::uint32_t>(values.size());
    double sum = 0.0;
    float  max = values.front();
    for (const float v : values) {
        sum += static_cast<double>(v);
        max = std::max(max, v);
    }
    s.avg = static_cast<float>(sum / static_cast<double>(values.size()));
    s.max = max;
    s.p50 = percentile(values, 50.0F);
    s.p95 = percentile(values, 95.0F);
    return s;
}

float SampleRing::at(std::uint32_t i) const {
    const auto cap = static_cast<std::uint32_t>(data_.size());
    // Oldest kept sample: head_ once the ring has wrapped, else index 0.
    const std::uint32_t oldest = size_ < cap ? 0 : head_;
    return data_[(oldest + i) % cap];
}

void SampleRing::copy_to(std::vector<float>& out) const {
    out.reserve(out.size() + size_);
    for (std::uint32_t i = 0; i < size_; ++i) out.push_back(at(i));
}

SampleStats SampleRing::stats() const {
    std::vector<float> scratch;
    copy_to(scratch);
    SampleStats s = compute_stats(scratch);
    s.last = latest();
    return s;
}

} // namespace engine::core
