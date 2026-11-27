#include "engine/core/frame_profiler.hpp"

#include <algorithm>
#include <utility>

namespace engine::core {

namespace {

constexpr std::size_t k_max_counters = 64;

[[nodiscard]] float ns_to_ms(std::uint64_t ns) {
    return static_cast<float>(static_cast<double>(ns) * 1e-6);
}

[[nodiscard]] float sort_value(const ZoneSummary& z, ZoneSort key) {
    switch (key) {
    case ZoneSort::cost: return std::max(z.cpu.avg, z.gpu.avg);
    case ZoneSort::cpu_avg: return z.cpu.avg;
    case ZoneSort::cpu_p95: return z.cpu.p95;
    case ZoneSort::cpu_max: return z.cpu.max;
    case ZoneSort::gpu_avg: return z.gpu.avg;
    case ZoneSort::gpu_p95: return z.gpu.p95;
    case ZoneSort::gpu_max: return z.gpu.max;
    case ZoneSort::name: return 0.0F;
    }
    return 0.0F;
}

} // namespace

void sort_zones(std::vector<ZoneSummary>& zones, ZoneSort key, bool ascending) {
    if (key == ZoneSort::name) {
        std::ranges::stable_sort(zones, [ascending](const ZoneSummary& l, const ZoneSummary& r) {
            return ascending ? l.name < r.name : l.name > r.name;
        });
        return;
    }
    std::ranges::stable_sort(zones, [key, ascending](const ZoneSummary& l, const ZoneSummary& r) {
        const float a = sort_value(l, key);
        const float b = sort_value(r, key);
        return ascending ? a < b : a > b;
    });
}

FrameProfiler::FrameProfiler(Config config)
    : config_(config),
      buffers_{ScopeBuffer(config.scope_capacity), ScopeBuffer(config.scope_capacity)},
      total_(config.window),
      cpu_(config.window),
      wait_(config.window),
      gpu_(config.window),
      sum_ms_(k_max_profile_names, 0.0),
      calls_(k_max_profile_names, 0) {
    touched_.reserve(k_max_profile_names);
    samples_.reserve(k_max_profile_names);
    counters_.reserve(k_max_counters);
}

FrameProfiler::~FrameProfiler() {
    unbind_current_thread();
}

void FrameProfiler::bind_current_thread() noexcept {
    detail::t_profile_recorder.buffer = &buffers_[current_];
    detail::t_profile_recorder.depth = 0;
    bound_ = true;
}

void FrameProfiler::unbind_current_thread() noexcept {
    detail::ThreadRecorder& r = detail::t_profile_recorder;
    if (r.buffer == &buffers_[0] || r.buffer == &buffers_[1]) {
        r.buffer = nullptr;
        r.depth = 0;
        bound_ = false;
    }
}

FrameProfiler::ZoneStats& FrameProfiler::stats_for(ProfileNameId id) {
    if (zones_.size() <= id) zones_.resize(static_cast<std::size_t>(id) + 1);
    ZoneStats& z = zones_[id];
    if (z.cpu.capacity() == 0) {
        z.cpu = SampleRing(config_.window);
        z.gpu = SampleRing(config_.window);
    }
    return z;
}

bool FrameProfiler::in_window(std::uint64_t frame) const {
    return frame + config_.window > frame_;
}

void FrameProfiler::end_frame() {
    const std::uint64_t now = profile_now_ns();
    ScopeBuffer& closed = buffers_[current_];

    // Sum each zone's time over its calls this frame.
    touched_.clear();
    double wait_ms = 0.0;
    for (const ScopeRecord& r : closed.records()) {
        const ProfileNameId id = r.name < k_max_profile_names ? r.name : k_profile_overflow_name;
        if (calls_[id] == 0) touched_.push_back(id);
        const double ms = static_cast<double>(r.end_ns - r.start_ns) * 1e-6;
        sum_ms_[id] += ms;
        ++calls_[id];
    }
    samples_.clear();
    for (const ProfileNameId id : touched_) {
        ZoneStats& z = stats_for(id);
        const auto ms = static_cast<float>(sum_ms_[id]);
        z.cpu.push(ms);
        z.last_cpu_frame = frame_;
        z.seen_cpu = true;
        z.calls = calls_[id];
        if (profile_category(id) == ProfileCategory::wait) wait_ms += sum_ms_[id];
        samples_.push_back({id, ms});
        sum_ms_[id] = 0.0;
        calls_[id] = 0;
    }

    if (last_end_ns_ != 0) {
        const float total = ns_to_ms(now - last_end_ns_);
        const float wait = std::min(static_cast<float>(wait_ms), total);
        total_.push(total);
        wait_.push(wait);
        cpu_.push(total - wait);
        captures_.on_cpu_frame(frame_, samples_, total, total - wait, wait);
    }
    last_end_ns_ = now;

    // The closed frame stays readable; the other buffer records the next one.
    current_ ^= 1U;
    buffers_[current_].clear();
    detail::ThreadRecorder& r = detail::t_profile_recorder;
    if (r.buffer == &closed) r.buffer = &buffers_[current_];
    ++frame_;
}

void FrameProfiler::submit_gpu_frame(std::uint64_t frame_index,
                                     std::span<const GpuZoneTiming> zones, float total_ms) {
    touched_.clear();
    for (const GpuZoneTiming& t : zones) {
        const ProfileNameId id = t.id < k_max_profile_names ? t.id : k_profile_overflow_name;
        if (calls_[id] == 0) touched_.push_back(id);
        sum_ms_[id] += static_cast<double>(t.ms);
        ++calls_[id];
    }
    samples_.clear();
    for (const ProfileNameId id : touched_) {
        ZoneStats& z = stats_for(id);
        const auto ms = static_cast<float>(sum_ms_[id]);
        z.gpu.push(ms);
        z.last_gpu_frame = frame_index;
        z.seen_gpu = true;
        samples_.push_back({id, ms});
        sum_ms_[id] = 0.0;
        calls_[id] = 0;
    }
    gpu_.push(total_ms);
    captures_.on_gpu_frame(frame_index, samples_, total_ms);
}

void FrameProfiler::set_gpu_status(std::string status, bool timing_available) {
    gpu_status_ = std::move(status);
    gpu_available_ = timing_available;
    captures_.set_gpu_expected(timing_available);
}

void FrameProfiler::set_counter(ProfileNameId id, double value) noexcept {
    for (Counter& c : counters_) {
        if (c.id == id) {
            c.value = value;
            return;
        }
    }
    if (counters_.size() < k_max_counters) counters_.push_back({id, value});
}

std::vector<ZoneSummary> FrameProfiler::zones() const {
    std::vector<ZoneSummary> out;
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        const ZoneStats& z = zones_[i];
        const bool cpu = z.seen_cpu && in_window(z.last_cpu_frame);
        const bool gpu = z.seen_gpu && in_window(z.last_gpu_frame);
        if (!cpu && !gpu) continue;
        const auto id = static_cast<ProfileNameId>(i);
        ZoneSummary s;
        s.id = id;
        s.name = profile_name(id);
        s.category = profile_category(id);
        if (cpu) s.cpu = z.cpu.stats();
        if (gpu) s.gpu = z.gpu.stats();
        s.calls = cpu ? z.calls : 0;
        out.push_back(s);
    }
    return out;
}

FrameSummary FrameProfiler::frame_summary() const {
    return FrameSummary{.total = total_.stats(),
                        .cpu = cpu_.stats(),
                        .wait = wait_.stats(),
                        .gpu = gpu_.stats()};
}

std::vector<ProfileCounter> FrameProfiler::counters() const {
    std::vector<ProfileCounter> out;
    out.reserve(counters_.size());
    for (const Counter& c : counters_) out.push_back({profile_name(c.id), c.value});
    return out;
}

void FrameProfiler::frame_history(std::vector<float>& total_ms, std::vector<float>& gpu_ms) const {
    total_ms.clear();
    gpu_ms.clear();
    total_.copy_to(total_ms);
    gpu_.copy_to(gpu_ms);
}

std::span<const ScopeRecord> FrameProfiler::last_frame_scopes() const {
    return buffers_[current_ ^ 1U].records();
}

std::uint32_t FrameProfiler::last_frame_dropped() const {
    return buffers_[current_ ^ 1U].dropped();
}

void FrameProfiler::reset() {
    zones_.clear();
    total_.clear();
    cpu_.clear();
    wait_.clear();
    gpu_.clear();
    counters_.clear();
    captures_.clear();
}

FrameProfiler& profiler() {
    static FrameProfiler instance;
    return instance;
}

} // namespace engine::core
