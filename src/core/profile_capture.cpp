#include "engine/core/profile_capture.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace engine::core {

namespace {

// Stats over the present (>= 0) entries of a per-frame series.
[[nodiscard]] SampleStats present_stats(const std::vector<float>& series, double& total) {
    std::vector<float> values;
    values.reserve(series.size());
    total = 0.0;
    for (const float v : series) {
        if (v < 0.0F) continue;
        values.push_back(v);
        total += static_cast<double>(v);
    }
    float last = 0.0F;
    if (!values.empty()) last = values.back();
    SampleStats s = compute_stats(values);
    s.last = last;
    return s;
}

// Self time ranks: an inclusive ranking puts every wrapper (update_world,
// record_frame) above the work nested inside it.
[[nodiscard]] double total_key(const CaptureZoneStats& z) {
    return std::max(z.cpu_self_total_ms, z.gpu_total_ms);
}
[[nodiscard]] float p95_key(const CaptureZoneStats& z) {
    return std::max(z.cpu_self.p95, z.gpu.p95);
}

} // namespace

std::expected<std::uint32_t, Error> ProfileCaptures::start(const CaptureRequest& request) {
    if (request.frames < 1 || request.frames > k_capture_max_frames) {
        return std::unexpected(Error{std::format("frames: expected 1..{}, got {}",
                                                 k_capture_max_frames, request.frames)});
    }
    if (!(request.budget_ms >= k_capture_min_budget_ms &&
          request.budget_ms <= k_capture_max_budget_ms)) {
        return std::unexpected(Error{std::format("budget_ms: expected {}..{} (milliseconds)",
                                                 k_capture_min_budget_ms,
                                                 k_capture_max_budget_ms)});
    }
    if (request.top < 1 || request.top > k_capture_max_top) {
        return std::unexpected(
            Error{std::format("top: expected 1..{}, got {}", k_capture_max_top, request.top)});
    }
    if (active_) {
        return std::unexpected(Error{std::format(
            "busy: capture {} is still recording ({}/{} frames); read it with profile.result "
            "{{\"id\": {}}} once it finishes",
            active_->id, active_->cpu_frames, active_->request.frames, active_->id)});
    }
    Active a;
    a.request = request;
    a.id = next_id_++;
    const std::size_t n = request.frames;
    a.total.assign(n, 0.0F);
    a.cpu.assign(n, 0.0F);
    a.wait.assign(n, 0.0F);
    a.gpu.assign(n, -1.0F);
    a.zone_index.assign(k_max_profile_names, -1);
    active_ = std::move(a);
    return active_->id;
}

ProfileCaptures::ZoneSeries& ProfileCaptures::series(Active& a, ProfileNameId id) {
    std::int16_t& index = a.zone_index[id < a.zone_index.size() ? id : k_profile_overflow_name];
    if (index < 0) {
        index = static_cast<std::int16_t>(a.zones.size());
        ZoneSeries s;
        s.id = id;
        s.cpu.assign(a.request.frames, -1.0F);
        s.cpu_self.assign(a.request.frames, -1.0F);
        s.gpu.assign(a.request.frames, -1.0F);
        a.zones.push_back(std::move(s));
    }
    return a.zones[static_cast<std::size_t>(index)];
}

void ProfileCaptures::on_cpu_frame(std::uint64_t frame, std::span<const ZoneSample> zones,
                                   float total_ms, float cpu_ms, float wait_ms) {
    if (!active_) return;
    Active& a = *active_;
    if (!a.started) {
        a.started = true;
        a.first_frame = frame;
    }
    if (a.cpu_frames < a.request.frames && frame == a.first_frame + a.cpu_frames) {
        const std::uint32_t k = a.cpu_frames++;
        a.total[k] = total_ms;
        a.cpu[k] = cpu_ms;
        a.wait[k] = wait_ms;
        for (const ZoneSample& z : zones) {
            ZoneSeries& zs = series(a, z.id);
            zs.cpu[k] = std::max(zs.cpu[k], 0.0F) + z.ms;
            zs.cpu_self[k] = std::max(zs.cpu_self[k], 0.0F) + z.self_ms;
        }
    } else if (a.cpu_frames >= a.request.frames) {
        ++a.grace;
    }
    const bool gpu_done = !gpu_expected_ || a.gpu_frames >= a.request.frames ||
                          a.grace >= k_capture_gpu_grace_frames;
    if (a.cpu_frames >= a.request.frames && gpu_done) finish();
}

void ProfileCaptures::on_gpu_frame(std::uint64_t frame, std::span<const ZoneSample> zones,
                                   float gpu_ms) {
    if (!active_ || !active_->started) return;
    Active& a = *active_;
    if (frame < a.first_frame || frame >= a.first_frame + a.request.frames) return;
    const auto k = static_cast<std::size_t>(frame - a.first_frame);
    if (a.gpu[k] >= 0.0F) return; // already timed
    a.gpu[k] = std::max(gpu_ms, 0.0F);
    ++a.gpu_frames;
    for (const ZoneSample& z : zones) {
        float& slot = series(a, z.id).gpu[k];
        slot = std::max(slot, 0.0F) + z.ms;
    }
    if (a.cpu_frames >= a.request.frames && a.gpu_frames >= a.request.frames) finish();
}

std::optional<CaptureProgress> ProfileCaptures::progress(std::uint32_t id) const {
    if (!active_ || active_->id != id) return std::nullopt;
    return CaptureProgress{.id = id,
                           .frames = active_->request.frames,
                           .frames_done = active_->cpu_frames,
                           .waiting_for_gpu = active_->cpu_frames >= active_->request.frames};
}

const CaptureSummary* ProfileCaptures::result(std::uint32_t id) const {
    for (const CaptureSummary& s : done_) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

void ProfileCaptures::clear() {
    active_.reset();
    done_.clear();
}

void ProfileCaptures::finish() {
    Active& a = *active_;
    const std::uint32_t n = a.request.frames;
    CaptureSummary s;
    s.id = a.id;
    s.frames = n;
    s.first_frame = a.first_frame;
    s.budget_ms = a.request.budget_ms;
    double unused = 0.0;
    s.total = present_stats(a.total, unused);
    s.cpu = present_stats(a.cpu, unused);
    s.wait = present_stats(a.wait, unused);
    s.gpu = present_stats(a.gpu, unused);
    s.gpu_frames = a.gpu_frames;
    s.frame_total_ms = a.total;
    s.frame_gpu_ms = a.gpu;

    std::vector<CaptureZoneStats> zones;
    zones.reserve(a.zones.size());
    for (const ZoneSeries& series : a.zones) {
        CaptureZoneStats z;
        z.id = series.id;
        z.name = std::string(profile_name(series.id));
        z.category = profile_category(series.id);
        z.cpu = present_stats(series.cpu, z.cpu_total_ms);
        z.cpu_self = present_stats(series.cpu_self, z.cpu_self_total_ms);
        z.gpu = present_stats(series.gpu, z.gpu_total_ms);
        for (std::uint32_t k = 0; k < n; ++k) {
            if (series.cpu[k] >= 0.0F || series.gpu[k] >= 0.0F) ++z.frames;
        }
        zones.push_back(std::move(z));
    }
    const std::size_t top = std::min<std::size_t>(a.request.top, zones.size());
    s.top_by_total = zones;
    std::ranges::stable_sort(s.top_by_total, [](const auto& l, const auto& r) {
        return total_key(l) > total_key(r);
    });
    s.top_by_total.resize(top);
    s.top_by_p95 = std::move(zones);
    std::ranges::stable_sort(s.top_by_p95,
                             [](const auto& l, const auto& r) { return p95_key(l) > p95_key(r); });
    s.top_by_p95.resize(top);

    // Spikes: wall time or GPU time over budget; the worst first.
    for (std::uint32_t k = 0; k < n; ++k) {
        const bool over = a.total[k] > s.budget_ms || a.gpu[k] > s.budget_ms;
        if (!over) continue;
        ++s.spike_count;
        CaptureSpike spike;
        spike.frame = a.first_frame + k;
        spike.total_ms = a.total[k];
        spike.cpu_ms = a.cpu[k];
        spike.gpu_ms = a.gpu[k];
        for (const ZoneSeries& series : a.zones) {
            const ProfileCategory category = profile_category(series.id);
            // A wait zone is the symptom of a GPU-bound frame, not a cause.
            if (category == ProfileCategory::wait) continue;
            if (series.cpu_self[k] > 0.0F) {
                spike.worst.push_back({std::string(profile_name(series.id)), series.cpu[k],
                                       series.cpu_self[k], false});
            }
            if (series.gpu[k] > 0.0F) {
                spike.worst.push_back({std::string(profile_name(series.id)), series.gpu[k],
                                       series.gpu[k], true});
            }
        }
        // By self time: one call chain (update_world > physics_system >
        // physics_step) must not fill every slot with the same 12 ms.
        std::ranges::sort(spike.worst,
                          [](const auto& l, const auto& r) { return l.self_ms > r.self_ms; });
        if (spike.worst.size() > k_capture_spike_zones) spike.worst.resize(k_capture_spike_zones);
        s.spikes.push_back(std::move(spike));
    }
    std::ranges::stable_sort(s.spikes, [](const CaptureSpike& l, const CaptureSpike& r) {
        return std::max(l.total_ms, l.gpu_ms) > std::max(r.total_ms, r.gpu_ms);
    });
    if (s.spikes.size() > k_capture_max_spikes) s.spikes.resize(k_capture_max_spikes);

    active_.reset();
    done_.push_back(std::move(s));
    while (done_.size() > k_capture_kept_results) done_.pop_front();
}

} // namespace engine::core
