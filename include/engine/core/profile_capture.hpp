#pragma once

// Frame captures of the built-in profiler (ROADMAP R4.3): record N frames,
// then summarise them — the zones that cost the most in total and at the 95th
// percentile, the frames over a time budget (spikes) with their worst zones,
// and per-frame totals. The remote profile.capture / profile.result methods
// and the quat-mcp profile_capture tool read these summaries.
//
// A capture runs across frames: start() returns its id at once, the frame
// profiler feeds every closed frame (on_cpu_frame) and every resolved GPU
// frame (on_gpu_frame, which arrives frames-in-flight frames later), and the
// summary is built once all frames are in — or a few frames after the CPU
// side finished when GPU timings stop arriving. Main thread only.

#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/error.hpp"
#include "engine/core/profile_names.hpp"
#include "engine/core/profile_stats.hpp"

namespace engine::core {

// One zone's time in one frame (several calls summed).
struct ZoneSample {
    ProfileNameId id = 0;
    float         ms = 0.0F;
};

inline constexpr std::uint32_t k_capture_max_frames = 1000;
inline constexpr std::uint32_t k_capture_max_top = 50;
inline constexpr float         k_capture_min_budget_ms = 0.1F;
inline constexpr float         k_capture_max_budget_ms = 1000.0F;
// Frames a finished CPU side waits for the GPU timings of its last frames.
inline constexpr std::uint32_t k_capture_gpu_grace_frames = 8;
// Finished summaries kept for profile.result (oldest dropped first).
inline constexpr std::size_t k_capture_kept_results = 8;
// Spikes listed in a summary (the worst ones; spike_count counts all).
inline constexpr std::size_t k_capture_max_spikes = 10;
// Zones named per spike.
inline constexpr std::size_t k_capture_spike_zones = 3;

struct CaptureRequest {
    std::uint32_t frames = 120;        // 1..k_capture_max_frames
    float         budget_ms = 16.667F; // a frame over this is a spike
    std::uint32_t top = 10;            // zones per ranking, 1..k_capture_max_top
};

struct CaptureZoneStats {
    ProfileNameId   id = 0;
    std::string     name;
    ProfileCategory category = ProfileCategory::scope;
    std::uint32_t   frames = 0; // frames the zone ran in
    double          cpu_total_ms = 0.0;
    double          gpu_total_ms = 0.0;
    SampleStats     cpu; // over the frames it ran in (count 0: no CPU time)
    SampleStats     gpu; // count 0: no GPU timing
};

struct CaptureSpikeZone {
    std::string name;
    float       ms = 0.0F;
    bool        gpu = false; // GPU time (else CPU)
};

struct CaptureSpike {
    std::uint64_t                 frame = 0;
    float                         total_ms = 0.0F; // wall time
    float                         cpu_ms = 0.0F;
    float                         gpu_ms = -1.0F;  // -1: no GPU timing for the frame
    std::vector<CaptureSpikeZone> worst;           // up to k_capture_spike_zones
};

struct CaptureSummary {
    std::uint32_t id = 0;
    std::uint32_t frames = 0;
    std::uint64_t first_frame = 0;
    float         budget_ms = 0.0F;
    SampleStats   total; // wall time per frame
    SampleStats   cpu;   // wall minus wait
    SampleStats   wait;  // fence / acquire / present waits
    SampleStats   gpu;   // over the frames with GPU timing
    std::uint32_t gpu_frames = 0;
    // Ranked by the larger of the CPU and GPU value (total, then p95).
    std::vector<CaptureZoneStats> top_by_total;
    std::vector<CaptureZoneStats> top_by_p95;
    std::uint32_t                 spike_count = 0;
    std::vector<CaptureSpike>     spikes; // worst first
    std::vector<float>            frame_total_ms; // per frame
    std::vector<float>            frame_gpu_ms;   // per frame, -1 without GPU timing
};

struct CaptureProgress {
    std::uint32_t id = 0;
    std::uint32_t frames = 0;
    std::uint32_t frames_done = 0;
    bool          waiting_for_gpu = false; // CPU side done, last GPU timings pending
};

class ProfileCaptures {
public:
    // Validates `request` and starts recording with the next closed frame.
    // One capture records at a time; finished ones stay readable.
    [[nodiscard]] std::expected<std::uint32_t, Error> start(const CaptureRequest& request);

    // A closed frame: its zones and its wall / CPU / wait time.
    void on_cpu_frame(std::uint64_t frame, std::span<const ZoneSample> zones, float total_ms,
                      float cpu_ms, float wait_ms);
    // GPU timings of an earlier frame.
    void on_gpu_frame(std::uint64_t frame, std::span<const ZoneSample> zones, float gpu_ms);
    // False when the device has no timestamps: captures finish without
    // waiting for GPU frames.
    void set_gpu_expected(bool expected) { gpu_expected_ = expected; }

    // The recording capture `id`, if it is the one recording.
    [[nodiscard]] std::optional<CaptureProgress> progress(std::uint32_t id) const;
    // The summary of finished capture `id`, or null (unknown, dropped or
    // still recording).
    [[nodiscard]] const CaptureSummary* result(std::uint32_t id) const;
    [[nodiscard]] bool recording() const { return active_.has_value(); }
    // Ids issued so far are 1..last_id().
    [[nodiscard]] std::uint32_t last_id() const { return next_id_ - 1; }

    // Drops the recording capture and every summary (ids keep counting).
    void clear();

private:
    struct ZoneSeries {
        ProfileNameId      id = 0;
        std::vector<float> cpu; // per frame, -1 when absent
        std::vector<float> gpu;
    };
    struct Active {
        CaptureRequest             request;
        std::uint32_t              id = 0;
        bool                       started = false;
        std::uint64_t              first_frame = 0;
        std::uint32_t              cpu_frames = 0;
        std::uint32_t              gpu_frames = 0;
        std::uint32_t              grace = 0;
        std::vector<float>         total, cpu, wait, gpu; // per frame (gpu -1 until timed)
        std::vector<ZoneSeries>    zones;
        std::vector<std::int16_t>  zone_index; // by name id, -1 = none yet
    };

    [[nodiscard]] ZoneSeries& series(Active& a, ProfileNameId id);
    void finish();

    std::optional<Active>      active_;
    std::deque<CaptureSummary> done_;
    std::uint32_t              next_id_ = 1;
    bool                       gpu_expected_ = true;
};

} // namespace engine::core
