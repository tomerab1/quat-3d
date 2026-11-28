#pragma once

// Built-in frame profiler (ROADMAP R4.2) — always compiled, Tracy or not.
//
// Records CPU scopes on the thread bound to it (the main thread: systems,
// render-graph recording, the frame's waits) into a fixed-capacity per-frame
// buffer: name id, start / end in nanoseconds, nesting depth and thread
// index, 24 bytes each, with no allocation and no lock on the hot path.
// Other threads' ProfileScopes are no-ops here (Tracy sees every thread).
//
// end_frame() closes a frame: zone times are summed per name and pushed into
// rolling windows (default 240 frames) next to the frame's wall, CPU (wall
// minus waits) and wait time. The GPU side arrives later through
// submit_gpu_frame() (per-pass timestamp results, rhi::GpuProfiler). Queries
// — per-zone avg / p50 / p95 / max for CPU and GPU, frame totals, counters,
// frame history — feed the editor's Profiler panel and the remote
// profile.* methods. Everything except the hot path is main-thread only.
//
// Instrument code through the macros in engine/core/profile.hpp, which feed
// this profiler and Tracy together.

#include <chrono>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/profile_capture.hpp"
#include "engine/core/profile_names.hpp"
#include "engine/core/profile_stats.hpp"

namespace engine::core {

struct ScopeRecord {
    std::uint64_t start_ns = 0;
    std::uint64_t end_ns = 0;
    ProfileNameId name = 0;
    std::uint8_t  depth = 0;
    std::uint8_t  thread = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(ScopeRecord) == 24, "keep scope records compact");

// One frame's scope records: fixed capacity, extra records are counted and
// dropped (a frame's totals must not be built from a partial overwrite).
class ScopeBuffer {
public:
    ScopeBuffer() = default;
    explicit ScopeBuffer(std::size_t capacity) : records_(capacity) {}

    void push(const ScopeRecord& record) noexcept {
        if (size_ < records_.size()) {
            records_[size_++] = record;
        } else {
            ++dropped_;
        }
    }
    void clear() noexcept {
        size_ = 0;
        dropped_ = 0;
    }
    [[nodiscard]] std::span<const ScopeRecord> records() const { return {records_.data(), size_}; }
    [[nodiscard]] std::size_t capacity() const { return records_.size(); }
    [[nodiscard]] std::uint32_t dropped() const { return dropped_; }

private:
    std::vector<ScopeRecord> records_;
    std::size_t              size_ = 0;
    std::uint32_t            dropped_ = 0;
};

[[nodiscard]] inline std::uint64_t profile_now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

namespace detail {
// The calling thread's recording target; null buffer = not recording.
struct ThreadRecorder {
    ScopeBuffer* buffer = nullptr;
    std::uint8_t depth = 0;
};
inline thread_local ThreadRecorder t_profile_recorder;
} // namespace detail

// RAII scope: records [construction, destruction) under `id` when the
// calling thread is bound to a FrameProfiler. Use the QUAT_ZONE macros.
class ProfileScope {
public:
    explicit ProfileScope(ProfileNameId id) noexcept : id_(id) {
        detail::ThreadRecorder& r = detail::t_profile_recorder;
        if (r.buffer == nullptr) return;
        recorder_ = &r;
        depth_ = r.depth++;
        start_ns_ = profile_now_ns();
    }
    ~ProfileScope() {
        if (recorder_ == nullptr) return;
        const std::uint64_t end = profile_now_ns();
        --recorder_->depth;
        if (recorder_->buffer != nullptr) {
            recorder_->buffer->push({.start_ns = start_ns_,
                                     .end_ns = end,
                                     .name = id_,
                                     .depth = depth_,
                                     .thread = profile_thread_index()});
        }
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
    ProfileScope(ProfileScope&&) = delete;
    ProfileScope& operator=(ProfileScope&&) = delete;

private:
    detail::ThreadRecorder* recorder_ = nullptr;
    std::uint64_t           start_ns_ = 0;
    ProfileNameId           id_ = 0;
    std::uint8_t            depth_ = 0;
};

struct GpuZoneTiming {
    ProfileNameId id = 0;
    float         ms = 0.0F;
};

// Rolling statistics of one zone over the window.
struct ZoneSummary {
    ProfileNameId    id = 0;
    std::string_view name;
    ProfileCategory  category = ProfileCategory::scope;
    // CPU times are per frame the zone ran in, over its last `window` runs.
    // `cpu` is inclusive (children included); `cpu_self` excludes the time of
    // zones nested inside it, so wrappers rank by their own work.
    SampleStats      cpu;           // count 0: no CPU samples in the window
    SampleStats      cpu_self;
    SampleStats      gpu;           // count 0: no GPU timing
    std::uint32_t    calls = 0;     // calls in the zone's latest frame
};

struct FrameSummary {
    SampleStats total; // wall time between end_frame calls
    SampleStats cpu;   // wall minus wait
    SampleStats wait;  // wait-category zones
    SampleStats gpu;   // first to last timestamp of the frame
};

struct ProfileCounter {
    std::string_view name;
    double           value = 0.0;
};

// Orders for zone tables (editor, remote). `cost` is the larger of the CPU
// self and GPU average: what the zone itself costs whichever side it runs
// on (a wrapper zone does not outrank the work nested inside it).
enum class ZoneSort : std::uint8_t {
    cost,
    self_avg,
    self_p95,
    self_max,
    cpu_avg,
    cpu_p95,
    cpu_max,
    gpu_avg,
    gpu_p95,
    gpu_max,
    name
};
// Sorts `zones` by `key`; numeric keys put the largest first unless
// `ascending`, names sort A..Z unless !ascending.
void sort_zones(std::vector<ZoneSummary>& zones, ZoneSort key, bool ascending = false);

class FrameProfiler {
public:
    struct Config {
        std::uint32_t window = 240;          // frames of rolling statistics
        std::uint32_t scope_capacity = 8192; // scope records per frame
    };

    FrameProfiler() : FrameProfiler(Config{}) {}
    explicit FrameProfiler(Config config);
    ~FrameProfiler();
    FrameProfiler(const FrameProfiler&) = delete;
    FrameProfiler& operator=(const FrameProfiler&) = delete;
    FrameProfiler(FrameProfiler&&) = delete;
    FrameProfiler& operator=(FrameProfiler&&) = delete;

    // Records the calling thread's ProfileScopes into this profiler (one
    // thread at a time; the destructor unbinds the calling thread).
    void bind_current_thread() noexcept;
    void unbind_current_thread() noexcept;

    // ---- Per frame -------------------------------------------------------------
    // Index of the frame being recorded (0-based, advanced by end_frame).
    [[nodiscard]] std::uint64_t frame_index() const { return frame_; }
    // Closes the frame being recorded. The first frame has no start time, so
    // frame-level times begin with the second.
    void end_frame();
    // GPU timings of frame `frame_index` (per zone; `total_ms` first to last
    // timestamp). Arrives frames-in-flight frames after that frame closed.
    void submit_gpu_frame(std::uint64_t frame_index, std::span<const GpuZoneTiming> zones,
                          float total_ms);
    // "ok", or why GPU timing is off ("unsupported: ...", "disabled").
    void set_gpu_status(std::string status, bool timing_available);
    [[nodiscard]] std::string_view gpu_status() const { return gpu_status_; }
    [[nodiscard]] bool gpu_timing_available() const { return gpu_available_; }
    // Per-frame counter (draws, triangles, memory, ...): the latest value.
    void set_counter(ProfileNameId id, double value) noexcept;

    // ---- Queries (main thread) ---------------------------------------------------
    [[nodiscard]] std::uint32_t window() const { return config_.window; }
    // Frames with frame-level samples in the window.
    [[nodiscard]] std::uint32_t frames_in_window() const { return total_.size(); }
    // Zones seen within the last window frames, in name-id order.
    [[nodiscard]] std::vector<ZoneSummary> zones() const;
    [[nodiscard]] FrameSummary frame_summary() const;
    [[nodiscard]] std::vector<ProfileCounter> counters() const;
    // Frame wall times and GPU times (ms) of the window, oldest first (GPU
    // lags by the frames in flight).
    void frame_history(std::vector<float>& total_ms, std::vector<float>& gpu_ms) const;
    // The last closed frame's scope records and how many were dropped.
    [[nodiscard]] std::span<const ScopeRecord> last_frame_scopes() const;
    [[nodiscard]] std::uint32_t last_frame_dropped() const;

    // Frame captures (profile.capture / profile.result).
    [[nodiscard]] ProfileCaptures& captures() { return captures_; }
    [[nodiscard]] const ProfileCaptures& captures() const { return captures_; }

    // Clears statistics, counters and captures (ids keep counting).
    void reset();

private:
    struct ZoneStats {
        SampleRing    cpu;
        SampleRing    cpu_self;
        SampleRing    gpu;
        std::uint64_t last_cpu_frame = 0;
        std::uint64_t last_gpu_frame = 0;
        bool          seen_cpu = false;
        bool          seen_gpu = false;
        std::uint32_t calls = 0;
    };
    [[nodiscard]] ZoneStats& stats_for(ProfileNameId id);
    [[nodiscard]] bool in_window(std::uint64_t frame) const;

    Config config_;
    // Double-buffered: the bound thread writes buffers_[current_]; the other
    // holds the last closed frame.
    ScopeBuffer   buffers_[2];
    std::uint32_t current_ = 0;
    bool          bound_ = false;

    std::uint64_t frame_ = 0;
    std::uint64_t last_end_ns_ = 0;

    std::vector<ZoneStats> zones_; // by name id (grown as names appear)
    SampleRing             total_, cpu_, wait_, gpu_;

    // end_frame / submit_gpu_frame scratch (sized to k_max_profile_names).
    std::vector<double>        sum_ms_;
    std::vector<double>        self_ms_;
    // Time of the closed children per nesting depth while a frame's records
    // are summed (records arrive in close order, children first).
    std::array<std::uint64_t, 257> child_ns_{};
    std::vector<std::uint32_t> calls_;
    std::vector<ProfileNameId> touched_;
    std::vector<ZoneSample>    samples_;

    struct Counter {
        ProfileNameId id = 0;
        double        value = 0.0;
    };
    std::vector<Counter> counters_; // capacity reserved; unique ids

    std::string     gpu_status_ = "not started";
    bool            gpu_available_ = false;
    ProfileCaptures captures_;
};

// The process-wide profiler the QUAT_* macros and the application use.
[[nodiscard]] FrameProfiler& profiler();

} // namespace engine::core
