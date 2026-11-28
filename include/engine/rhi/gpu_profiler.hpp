#pragma once

// GPU zone timing (ROADMAP R4.2): one timestamp query pool per frame in
// flight, written with vkCmdWriteTimestamp2 around every render-graph pass and
// read back without stalling once the slot's fence has been waited (results
// are 64-bit and availability-checked; a frame whose queries are not all
// available is skipped, never waited for).
//
// Timestamps are taken at ALL_COMMANDS: a zone starts when everything before
// it finished and ends when its own work did, so sibling zones do not overlap
// and their sum approaches the frame's GPU time. Values are masked to the
// queue's timestampValidBits and scaled by timestampPeriod. Devices without
// timestamps on the graphics queue (timestampValidBits 0) get a disabled
// profiler whose calls do nothing; status() says why. MoltenVK: Apple GPUs
// sample timestamps at Metal encoder boundaries, so passes recorded into
// one encoder can read as 0 ms there (their sum stays right).
//
// With QUAT_TRACY (ENGINE_PROFILE) the zones are also sent to Tracy through a
// TracyVkContext, calibrated against the CPU clock when the device offers
// VK_KHR/EXT_calibrated_timestamps. No Vulkan query handle and no Tracy type
// appears here.
//
// Frame flow (main thread, per frame-in-flight slot):
//   collect(slot)            after the slot's fence: the frame it last timed
//   begin_frame(cmd, slot)   first command of the frame's command buffer
//   begin_zone / end_zone    around each pass (RenderGraph::execute does it)
//   end_frame(cmd)           last command before vkEndCommandBuffer

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/core/frame_profiler.hpp"

namespace engine::rhi {

class Device;

// One resolved zone of a collected frame.
struct GpuZoneResult {
    core::ProfileNameId id = 0;
    float               start_ms = 0.0F; // from the frame's first timestamp
    float               ms = 0.0F;
    std::uint8_t        depth = 0;
};

struct GpuFrameResult {
    std::uint64_t                  frame_index = 0; // as given to begin_frame
    float                          total_ms = 0.0F; // begin_frame to end_frame
    std::span<const GpuZoneResult> zones;           // valid until the next collect()
};

class GpuProfiler {
public:
    struct Config {
        std::uint32_t frames_in_flight = 2;
        std::uint32_t max_zones = 256; // per frame; more are not timed
        bool          tracy = true;    // feed Tracy when compiled in
    };

    // Handle returned by begin_zone; pass it to end_zone.
    using ZoneToken = std::uint32_t;
    static constexpr ZoneToken k_no_zone = UINT32_MAX;

    GpuProfiler();
    ~GpuProfiler();
    GpuProfiler(GpuProfiler&&) noexcept;
    GpuProfiler& operator=(GpuProfiler&&) noexcept;
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // A disabled profiler (supported() false, status() says why) when the
    // graphics queue has no timestamps; an error only when Vulkan fails.
    [[nodiscard]] static std::expected<GpuProfiler, core::Error> create(const Device& device,
                                                                        Config config);
    [[nodiscard]] static std::expected<GpuProfiler, core::Error> create(const Device& device) {
        return create(device, Config{});
    }

    [[nodiscard]] bool supported() const;
    // "ok (…)" or the reason timing is off.
    [[nodiscard]] std::string_view status() const;
    // Nanoseconds per timestamp tick, and the valid bits of the queue's
    // timestamps (0 when unsupported).
    [[nodiscard]] float period_ns() const;
    [[nodiscard]] std::uint32_t valid_bits() const;
    [[nodiscard]] bool tracy_active() const;

    // The frame timed in `slot` before, if all its queries are available.
    // Call after waiting for the slot's fence; never blocks. Each frame is
    // returned once.
    [[nodiscard]] std::optional<GpuFrameResult> collect(std::uint32_t slot);
    // Resets the slot's queries and writes the frame's first timestamp.
    void begin_frame(VkCommandBuffer cmd, std::uint32_t slot, std::uint64_t frame_index);
    [[nodiscard]] ZoneToken begin_zone(VkCommandBuffer cmd, core::ProfileNameId id,
                                       const char* name);
    void end_zone(VkCommandBuffer cmd, ZoneToken token);
    void end_frame(VkCommandBuffer cmd);

    // Tears down the query pools (and the Tracy context); the device must be
    // idle. Idempotent; the destructor calls it.
    void destroy() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// RAII zone for code outside the render graph (e.g. the skinning dispatch).
class GpuZoneScope {
public:
    GpuZoneScope(GpuProfiler* profiler, VkCommandBuffer cmd, core::ProfileNameId id,
                 const char* name)
        : profiler_(profiler), cmd_(cmd),
          token_(profiler != nullptr ? profiler->begin_zone(cmd, id, name)
                                     : GpuProfiler::k_no_zone) {}
    ~GpuZoneScope() {
        if (profiler_ != nullptr) profiler_->end_zone(cmd_, token_);
    }
    GpuZoneScope(const GpuZoneScope&) = delete;
    GpuZoneScope& operator=(const GpuZoneScope&) = delete;
    GpuZoneScope(GpuZoneScope&&) = delete;
    GpuZoneScope& operator=(GpuZoneScope&&) = delete;

private:
    GpuProfiler*           profiler_;
    VkCommandBuffer        cmd_;
    GpuProfiler::ZoneToken token_;
};

} // namespace engine::rhi
