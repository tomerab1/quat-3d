#include "engine/rhi/gpu_profiler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"

#if defined(QUAT_TRACY)
#include <tracy/TracyVulkan.hpp>
#endif

namespace engine::rhi {

namespace {

// Query 0: the frame's first timestamp, query 1: its last; zones follow in
// begin / end pairs.
constexpr std::uint32_t k_frame_queries = 2;
constexpr std::uint32_t k_max_depth = 16;

[[nodiscard]] std::uint32_t graphics_timestamp_bits(const Device& device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device(), &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device(), &count, families.data());
    const std::uint32_t family = device.queue_families().graphics;
    return family < families.size() ? families[family].timestampValidBits : 0;
}

} // namespace

struct GpuProfiler::Impl {
    struct Zone {
        core::ProfileNameId id = 0;
        std::uint32_t       begin = 0; // query index
        std::uint32_t       end = 0;
        std::uint8_t        depth = 0;
    };
    struct Slot {
        VkQueryPool       pool = VK_NULL_HANDLE;
        bool              pending = false; // timed a frame not yet collected
        std::uint64_t     frame_index = 0;
        std::uint32_t     used = 0;        // queries written
        std::vector<Zone> zones;
    };

    VkDevice          device = VK_NULL_HANDLE;
    Config            config;
    std::uint32_t     queries_per_pool = 0;
    std::uint32_t     valid_bits = 0;
    std::uint64_t     mask = 0;
    float             period_ns = 0.0F;
    bool              supported = false;
    std::string       status;
    std::vector<Slot> slots;

    // The frame being recorded.
    Slot*         current = nullptr;
    std::uint8_t  depth = 0;
    std::array<std::uint32_t, k_max_depth> open{}; // zone index per open depth

    std::vector<std::uint64_t> raw; // value + availability per query
    std::vector<GpuZoneResult> results;

#if defined(QUAT_TRACY)
    tracy::VkCtx* tracy_ctx = nullptr;
    // Tracy's GPU scopes are RAII objects; begin_zone / end_zone construct
    // and destroy them here, one per open depth.
    struct TracyScopeStorage {
        alignas(tracy::VkCtxScope) std::array<std::byte, sizeof(tracy::VkCtxScope)> bytes;
        bool live = false;
    };
    std::array<TracyScopeStorage, k_max_depth> tracy_scopes{};

    void create_tracy(const Device& dev);
#endif

    [[nodiscard]] float ticks_to_ms(std::uint64_t ticks) const {
        return static_cast<float>(static_cast<double>(ticks & mask) *
                                  static_cast<double>(period_ns) * 1e-6);
    }
    void write(VkCommandBuffer cmd, std::uint32_t query) const {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, query);
    }
    // Closes the open zone at `at`: its end timestamp, and its Tracy scope
    // (whose destructor records Tracy's end query into the same command
    // buffer).
    void close(VkCommandBuffer cmd, std::uint8_t at) {
        if (current != nullptr && open[at] != UINT32_MAX) write(cmd, current->zones[open[at]].end);
        open[at] = UINT32_MAX;
#if defined(QUAT_TRACY)
        TracyScopeStorage& storage = tracy_scopes[at];
        if (storage.live) {
            std::destroy_at(reinterpret_cast<tracy::VkCtxScope*>(storage.bytes.data()));
            storage.live = false;
        }
#endif
    }
    // Closes every zone deeper than `to`, innermost first.
    void unwind_to(VkCommandBuffer cmd, std::uint8_t to) {
        while (depth > to) {
            --depth;
            close(cmd, depth);
        }
    }
    void destroy() noexcept;
};

#if defined(QUAT_TRACY)
void GpuProfiler::Impl::create_tracy(const Device& dev) {
    // TracyVkContext submits a few one-time commands to calibrate; it gets
    // its own transient pool on the graphics family.
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = dev.queue_families().graphics;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev.handle(), &pool_info, nullptr, &cmd_pool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev.handle(), &alloc, &cmd) == VK_SUCCESS) {
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains = nullptr;
        PFN_vkGetCalibratedTimestampsEXT timestamps = nullptr;
        const bool khr = dev.calibrated_timestamps() == Device::CalibratedTimestamps::khr;
        if (dev.calibrated_timestamps() != Device::CalibratedTimestamps::none) {
            domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
                vkGetInstanceProcAddr(dev.instance(),
                                      khr ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"
                                          : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
            timestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                vkGetDeviceProcAddr(dev.handle(), khr ? "vkGetCalibratedTimestampsKHR"
                                                      : "vkGetCalibratedTimestampsEXT"));
        }
        if (domains != nullptr && timestamps != nullptr) {
            tracy_ctx = TracyVkContextCalibrated(dev.physical_device(), dev.handle(),
                                                 dev.graphics_queue(), cmd, domains, timestamps);
        } else {
            tracy_ctx = TracyVkContext(dev.physical_device(), dev.handle(), dev.graphics_queue(),
                                       cmd);
        }
        if (tracy_ctx != nullptr) {
            constexpr std::string_view name = "graphics queue";
            TracyVkContextName(tracy_ctx, name.data(), static_cast<std::uint16_t>(name.size()));
        }
    }
    vkDestroyCommandPool(dev.handle(), cmd_pool, nullptr);
}
#endif

void GpuProfiler::Impl::destroy() noexcept {
#if defined(QUAT_TRACY)
    for (TracyScopeStorage& s : tracy_scopes) {
        if (s.live) std::destroy_at(reinterpret_cast<tracy::VkCtxScope*>(s.bytes.data()));
        s.live = false;
    }
    if (tracy_ctx != nullptr) {
        TracyVkDestroy(tracy_ctx);
        tracy_ctx = nullptr;
    }
#endif
    for (Slot& s : slots) {
        if (s.pool != VK_NULL_HANDLE) vkDestroyQueryPool(device, s.pool, nullptr);
        s.pool = VK_NULL_HANDLE;
    }
    slots.clear();
    current = nullptr;
}

GpuProfiler::GpuProfiler() = default;
GpuProfiler::~GpuProfiler() {
    destroy();
}
GpuProfiler::GpuProfiler(GpuProfiler&&) noexcept = default;
GpuProfiler& GpuProfiler::operator=(GpuProfiler&& other) noexcept {
    if (this != &other) {
        destroy();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

void GpuProfiler::destroy() noexcept {
    if (impl_) impl_->destroy();
}

std::expected<GpuProfiler, core::Error> GpuProfiler::create(const Device& device, Config config) {
    GpuProfiler out;
    out.impl_ = std::make_unique<Impl>();
    Impl& m = *out.impl_;
    m.device = device.handle();
    m.config = config;
    m.config.frames_in_flight = std::max(config.frames_in_flight, 1U);
    m.valid_bits = graphics_timestamp_bits(device);
    m.period_ns = device.properties().limits.timestampPeriod;
    if (m.valid_bits == 0 || !(m.period_ns > 0.0F)) {
        m.status = "unsupported: the graphics queue has no timestamps (timestampValidBits 0)";
        return out;
    }
    m.mask = m.valid_bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << m.valid_bits) - 1;
    m.queries_per_pool = k_frame_queries + 2 * config.max_zones;

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = m.queries_per_pool;
    m.slots.resize(m.config.frames_in_flight);
    for (Impl::Slot& s : m.slots) {
        if (VkResult r = vkCreateQueryPool(m.device, &info, nullptr, &s.pool); r != VK_SUCCESS) {
            m.destroy();
            return std::unexpected(
                core::Error{std::format("GpuProfiler: vkCreateQueryPool failed ({})",
                                        static_cast<int>(r))});
        }
        s.zones.reserve(config.max_zones);
    }
    m.raw.resize(static_cast<std::size_t>(m.queries_per_pool) * 2);
    m.results.reserve(config.max_zones);
    m.supported = true;
    m.status = std::format("ok ({} valid bits, {:.3f} ns per tick)", m.valid_bits, m.period_ns);
#if defined(QUAT_TRACY)
    if (config.tracy) {
        m.create_tracy(device);
        if (m.tracy_ctx != nullptr) {
            m.status += device.calibrated_timestamps() != Device::CalibratedTimestamps::none
                            ? ", Tracy GPU context (calibrated)"
                            : ", Tracy GPU context";
        }
    }
#endif
    return out;
}

bool GpuProfiler::supported() const {
    return impl_ && impl_->supported;
}
std::string_view GpuProfiler::status() const {
    return impl_ ? std::string_view(impl_->status) : std::string_view("not created");
}
float GpuProfiler::period_ns() const {
    return impl_ ? impl_->period_ns : 0.0F;
}
std::uint32_t GpuProfiler::valid_bits() const {
    return impl_ ? impl_->valid_bits : 0;
}
bool GpuProfiler::tracy_active() const {
#if defined(QUAT_TRACY)
    return impl_ && impl_->tracy_ctx != nullptr;
#else
    return false;
#endif
}

std::optional<GpuFrameResult> GpuProfiler::collect(std::uint32_t slot) {
    if (!supported() || slot >= impl_->slots.size()) return std::nullopt;
    Impl& m = *impl_;
    Impl::Slot& s = m.slots[slot];
    if (!s.pending || s.used < k_frame_queries) return std::nullopt;
    s.pending = false;
    // No WAIT bit: the caller waited for the slot's fence; anything still
    // unavailable (a frame that never reached the queue) is dropped.
    const VkResult r = vkGetQueryPoolResults(
        m.device, s.pool, 0, s.used, static_cast<std::size_t>(s.used) * 2 * sizeof(std::uint64_t),
        m.raw.data(), 2 * sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return std::nullopt;
    const auto value = [&](std::uint32_t q) { return m.raw[static_cast<std::size_t>(q) * 2]; };
    const auto available = [&](std::uint32_t q) {
        return m.raw[static_cast<std::size_t>(q) * 2 + 1] != 0;
    };
    if (!available(0) || !available(1)) return std::nullopt;
    const std::uint64_t frame_begin = value(0);

    m.results.clear();
    for (const Impl::Zone& z : s.zones) {
        if (!available(z.begin) || !available(z.end)) continue;
        m.results.push_back(GpuZoneResult{.id = z.id,
                                          .start_ms = m.ticks_to_ms(value(z.begin) - frame_begin),
                                          .ms = m.ticks_to_ms(value(z.end) - value(z.begin)),
                                          .depth = z.depth});
    }
    return GpuFrameResult{.frame_index = s.frame_index,
                          .total_ms = m.ticks_to_ms(value(1) - frame_begin),
                          .zones = m.results};
}

void GpuProfiler::begin_frame(VkCommandBuffer cmd, std::uint32_t slot, std::uint64_t frame_index) {
    if (!impl_) return;
    Impl& m = *impl_;
#if defined(QUAT_TRACY)
    if (m.tracy_ctx != nullptr) TracyVkCollect(m.tracy_ctx, cmd);
#endif
    // end_frame closes every open zone; a frame recorded without it leaves
    // zones open on a command buffer that is gone, so they are dropped
    // (never closed into the new one).
    m.depth = 0;
    m.open.fill(UINT32_MAX);
#if defined(QUAT_TRACY)
    for (Impl::TracyScopeStorage& storage : m.tracy_scopes) storage.live = false;
#endif
    if (!m.supported || slot >= m.slots.size()) {
        m.current = nullptr;
        return;
    }
    Impl::Slot& s = m.slots[slot];
    vkCmdResetQueryPool(cmd, s.pool, 0, m.queries_per_pool);
    s.pending = false;
    s.frame_index = frame_index;
    s.zones.clear();
    s.used = k_frame_queries;
    m.current = &s;
    m.write(cmd, 0);
}

GpuProfiler::ZoneToken GpuProfiler::begin_zone(VkCommandBuffer cmd, core::ProfileNameId id,
                                               const char* name) {
    if (!impl_) return k_no_zone;
    Impl& m = *impl_;
    const std::uint8_t depth = m.depth;
    if (depth >= k_max_depth) return k_no_zone;
    ++m.depth;
#if defined(QUAT_TRACY)
    if (m.tracy_ctx != nullptr && name != nullptr) {
        constexpr std::string_view file = __FILE__;
        constexpr std::string_view function = "RenderGraph pass";
        Impl::TracyScopeStorage& storage = m.tracy_scopes[depth];
        std::construct_at(reinterpret_cast<tracy::VkCtxScope*>(storage.bytes.data()),
                          m.tracy_ctx, static_cast<std::uint32_t>(__LINE__), file.data(),
                          file.size(), function.data(), function.size(), name,
                          std::char_traits<char>::length(name), cmd, true);
        storage.live = true;
    }
#else
    static_cast<void>(name);
#endif
    m.open[depth] = UINT32_MAX;
    if (m.current == nullptr || m.current->zones.size() >= m.config.max_zones) return depth;
    Impl::Slot& s = *m.current;
    const std::uint32_t begin = s.used;
    s.used += 2;
    m.open[depth] = static_cast<std::uint32_t>(s.zones.size());
    s.zones.push_back(Impl::Zone{.id = id, .begin = begin, .end = begin + 1, .depth = depth});
    m.write(cmd, begin);
    return depth;
}

void GpuProfiler::end_zone(VkCommandBuffer cmd, ZoneToken token) {
    if (!impl_ || token == k_no_zone) return;
    Impl& m = *impl_;
    // Already closed (an outer zone ended first and unwound it).
    if (token >= m.depth) return;
    // Zones nest: ending an outer zone first also closes the ones opened
    // inside it, so no timestamp or Tracy scope is left open.
    m.unwind_to(cmd, static_cast<std::uint8_t>(token));
}

void GpuProfiler::end_frame(VkCommandBuffer cmd) {
    if (!impl_) return;
    Impl& m = *impl_;
    // Zones still open (a caller that never ended them) close here, while
    // `cmd` still records: closing them in the next begin_frame would write
    // into a command buffer that was already submitted.
    m.unwind_to(cmd, 0);
    if (m.current == nullptr) return;
    m.write(cmd, 1);
    m.current->pending = true;
    m.current = nullptr;
}

} // namespace engine::rhi
