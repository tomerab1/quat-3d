// Profiling (ROADMAP R4): profile.frame, profile.capture / profile.result,
// profile.counters and profile.reset over the built-in frame profiler
// (engine/core/frame_profiler.hpp). Replies are compact — models read them —
// with times in milliseconds rounded to microseconds.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/frame_profiler.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr int k_default_zone_limit = 30;
constexpr int k_max_zone_limit = 200;
constexpr int k_default_capture_frames = 120;
constexpr float k_default_budget_ms = 16.667F;
constexpr int k_default_top = 10;
constexpr double k_mib = 1024.0 * 1024.0;

[[nodiscard]] double ms3(float v) {
    return std::round(static_cast<double>(v) * 1000.0) / 1000.0;
}
[[nodiscard]] double ms2(float v) {
    return std::round(static_cast<double>(v) * 100.0) / 100.0;
}
[[nodiscard]] double mib(std::uint64_t bytes) {
    return std::round(static_cast<double>(bytes) / k_mib * 10.0) / 10.0;
}

// {avg, p50, p95, max}, or null without samples.
[[nodiscard]] Json frame_stats_json(const core::SampleStats& s) {
    if (s.count == 0) return nullptr;
    return Json{{"avg", ms3(s.avg)}, {"p50", ms3(s.p50)}, {"p95", ms3(s.p95)}, {"max", ms3(s.max)}};
}
// {avg, p95, max}: zone rows drop the median to stay short.
[[nodiscard]] Json zone_stats_json(const core::SampleStats& s) {
    return Json{{"avg", ms3(s.avg)}, {"p95", ms3(s.p95)}, {"max", ms3(s.max)}};
}

constexpr core::EnumName<core::ZoneSort> k_sort_names[] = {
    {"cost", core::ZoneSort::cost},       {"self", core::ZoneSort::self_avg},
    {"self_p95", core::ZoneSort::self_p95}, {"self_max", core::ZoneSort::self_max},
    {"cpu", core::ZoneSort::cpu_avg},
    {"cpu_p95", core::ZoneSort::cpu_p95}, {"cpu_max", core::ZoneSort::cpu_max},
    {"gpu", core::ZoneSort::gpu_avg},     {"gpu_p95", core::ZoneSort::gpu_p95},
    {"gpu_max", core::ZoneSort::gpu_max}, {"name", core::ZoneSort::name},
};

// Zone-kind filter: a ProfileCategory, or all of them.
enum class KindFilter : std::uint8_t { all, scope, system, pass, wait };
constexpr core::EnumName<KindFilter> k_kind_names[] = {
    {"all", KindFilter::all},       {"scope", KindFilter::scope}, {"system", KindFilter::system},
    {"pass", KindFilter::pass},     {"wait", KindFilter::wait},
};

[[nodiscard]] bool kind_matches(KindFilter filter, core::ProfileCategory category) {
    switch (filter) {
    case KindFilter::all: return true;
    case KindFilter::scope: return category == core::ProfileCategory::scope;
    case KindFilter::system: return category == core::ProfileCategory::system;
    case KindFilter::pass: return category == core::ProfileCategory::pass;
    case KindFilter::wait: return category == core::ProfileCategory::wait;
    }
    return true;
}

[[nodiscard]] std::expected<core::FrameProfiler*, RpcError> profiler_of(Context& ctx) {
    if (ctx.services.profiler == nullptr) return unavailable("the frame profiler");
    return ctx.services.profiler;
}

[[nodiscard]] Json counters_json(const core::FrameProfiler& p) {
    Json out = Json::object();
    for (const core::ProfileCounter& c : p.counters()) {
        out[std::string(c.name)] = std::round(c.value * 1000.0) / 1000.0;
    }
    return out;
}

[[nodiscard]] Json frame_ms_json(const core::FrameSummary& f) {
    return Json{{"total", frame_stats_json(f.total)},
                {"cpu", frame_stats_json(f.cpu)},
                {"gpu", frame_stats_json(f.gpu)},
                {"wait", frame_stats_json(f.wait)}};
}

RpcResult profile_frame(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    core::ZoneSort sort = core::ZoneSort::cost;
    KindFilter kind = KindFilter::all;
    int limit = k_default_zone_limit;
    f.enumeration("sort", sort, k_sort_names)
        .enumeration("kind", kind, k_kind_names)
        .integer("limit", limit, 1, k_max_zone_limit);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto p = profiler_of(ctx);
    if (!p) return std::unexpected(p.error());
    const core::FrameProfiler& profiler = **p;

    std::vector<core::ZoneSummary> zones = profiler.zones();
    std::erase_if(zones, [kind](const core::ZoneSummary& z) {
        return !kind_matches(kind, z.category);
    });
    core::sort_zones(zones, sort, /*ascending=*/sort == core::ZoneSort::name);
    const std::size_t matched = zones.size();
    if (zones.size() > static_cast<std::size_t>(limit)) zones.resize(static_cast<std::size_t>(limit));

    Json rows = Json::array();
    for (const core::ZoneSummary& z : zones) {
        Json row{{"name", z.name}, {"kind", std::string(core::to_string(z.category))}};
        if (z.cpu.count > 0) {
            row["cpu"] = zone_stats_json(z.cpu);           // inclusive
            row["cpu_self"] = zone_stats_json(z.cpu_self); // nested zones excluded
            row["calls"] = z.calls;
        }
        if (z.gpu.count > 0) row["gpu"] = zone_stats_json(z.gpu);
        rows.push_back(std::move(row));
    }
    const core::FrameSummary frame = profiler.frame_summary();
    return Json{{"frame", profiler.frame_index()},
                {"window", profiler.window()},
                {"frames", profiler.frames_in_window()},
                {"fps", frame.total.avg > 0.0F ? std::round(1000.0 / frame.total.avg * 10.0) / 10.0
                                               : 0.0},
                {"frame_ms", frame_ms_json(frame)},
                {"gpu_timing", std::string(profiler.gpu_status())},
                {"zones", std::move(rows)},
                {"zones_matched", matched},
                {"counters", counters_json(profiler)}};
}

RpcResult profile_capture(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    core::CaptureRequest request{.frames = static_cast<std::uint32_t>(k_default_capture_frames),
                                 .budget_ms = k_default_budget_ms,
                                 .top = static_cast<std::uint32_t>(k_default_top)};
    f.integer("frames", request.frames, 1U, core::k_capture_max_frames)
        .number("budget_ms", request.budget_ms, core::k_capture_min_budget_ms,
                core::k_capture_max_budget_ms)
        .integer("top", request.top, 1U, core::k_capture_max_top);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto p = profiler_of(ctx);
    if (!p) return std::unexpected(p.error());
    const auto id = (*p)->captures().start(request);
    if (!id) return rpc_fail(rpc_code::failed, "profile.capture: " + id.error().message);
    return Json{{"id", *id},
                {"status", "recording"},
                {"frames", request.frames},
                {"budget_ms", ms3(request.budget_ms)},
                {"gpu_timing", (*p)->gpu_timing_available()}};
}

[[nodiscard]] Json capture_zone_json(const core::CaptureZoneStats& z) {
    Json row{{"name", z.name},
             {"kind", std::string(core::to_string(z.category))},
             {"frames", z.frames}};
    const auto side = [](double total, const core::SampleStats& s) {
        return Json{{"total", std::round(total * 1000.0) / 1000.0},
                    {"avg", ms3(s.avg)},
                    {"p95", ms3(s.p95)},
                    {"max", ms3(s.max)}};
    };
    if (z.cpu.count > 0) {
        row["cpu"] = side(z.cpu_total_ms, z.cpu);
        row["cpu_self"] = side(z.cpu_self_total_ms, z.cpu_self);
    }
    if (z.gpu.count > 0) row["gpu"] = side(z.gpu_total_ms, z.gpu);
    return row;
}

[[nodiscard]] Json summary_json(const core::CaptureSummary& s) {
    Json by_total = Json::array();
    for (const auto& z : s.top_by_total) by_total.push_back(capture_zone_json(z));
    Json by_p95 = Json::array();
    for (const auto& z : s.top_by_p95) by_p95.push_back(capture_zone_json(z));
    Json spikes = Json::array();
    for (const core::CaptureSpike& spike : s.spikes) {
        Json zones = Json::array();
        for (const core::CaptureSpikeZone& z : spike.worst) {
            zones.push_back({{"name", z.name},
                             {"self_ms", ms3(z.self_ms)},
                             {"ms", ms3(z.ms)},
                             {"on", z.gpu ? "gpu" : "cpu"}});
        }
        spikes.push_back({{"frame", spike.frame},
                          {"total_ms", ms3(spike.total_ms)},
                          {"cpu_ms", ms3(spike.cpu_ms)},
                          {"gpu_ms", spike.gpu_ms < 0.0F ? Json(nullptr) : Json(ms3(spike.gpu_ms))},
                          {"zones", std::move(zones)}});
    }
    Json total_ms = Json::array();
    for (const float v : s.frame_total_ms) total_ms.push_back(ms2(v));
    Json gpu_ms = Json::array();
    for (const float v : s.frame_gpu_ms) gpu_ms.push_back(v < 0.0F ? Json(nullptr) : Json(ms2(v)));
    return Json{{"id", s.id},
                {"status", "done"},
                {"frames", s.frames},
                {"first_frame", s.first_frame},
                {"budget_ms", ms3(s.budget_ms)},
                {"frame_ms",
                 {{"total", frame_stats_json(s.total)},
                  {"cpu", frame_stats_json(s.cpu)},
                  {"gpu", frame_stats_json(s.gpu)},
                  {"wait", frame_stats_json(s.wait)}}},
                {"gpu_frames", s.gpu_frames},
                {"top_by_total", std::move(by_total)},
                {"top_by_p95", std::move(by_p95)},
                {"spikes", {{"count", s.spike_count}, {"worst", std::move(spikes)}}},
                {"per_frame", {{"total_ms", std::move(total_ms)}, {"gpu_ms", std::move(gpu_ms)}}}};
}

RpcResult profile_result(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::int64_t id = 0;
    f.integer("id", id, std::int64_t{1}, std::int64_t{UINT32_MAX});
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (id == 0) return bad_params("id: required (the capture id profile.capture returned)");
    auto p = profiler_of(ctx);
    if (!p) return std::unexpected(p.error());
    const core::ProfileCaptures& captures = (*p)->captures();
    const auto capture_id = static_cast<std::uint32_t>(id);
    if (const core::CaptureSummary* s = captures.result(capture_id)) return summary_json(*s);
    if (const auto progress = captures.progress(capture_id)) {
        return Json{{"id", progress->id},
                    {"status", "recording"},
                    {"frames", progress->frames},
                    {"frames_done", progress->frames_done},
                    {"waiting_for_gpu", progress->waiting_for_gpu}};
    }
    std::string message = "capture " + std::to_string(id) + " not found: ";
    if (captures.last_id() == 0) {
        message += "no capture was started (profile.capture starts one)";
    } else {
        message += "ids 1.." + std::to_string(captures.last_id()) +
                   " were issued and the last " + std::to_string(core::k_capture_kept_results) +
                   " finished summaries are kept (profile.reset drops them)";
    }
    return rpc_fail(rpc_code::not_found, std::move(message));
}

template <typename Component>
[[nodiscard]] std::size_t count_of(const entt::registry& r) {
    return r.view<const Component>().size();
}

RpcResult profile_counters(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto p = profiler_of(ctx);
    if (!p) return std::unexpected(p.error());

    Json memory = nullptr;
    if (ctx.services.gpu_memory) {
        const std::vector<GpuMemoryHeap> heaps = ctx.services.gpu_memory();
        if (!heaps.empty()) {
            Json rows = Json::array();
            std::uint64_t local_usage = 0;
            std::uint64_t local_budget = 0;
            for (const GpuMemoryHeap& h : heaps) {
                rows.push_back({{"heap", h.heap},
                                {"device_local", h.device_local},
                                {"size_mib", mib(h.size)},
                                {"budget_mib", mib(h.budget)},
                                {"usage_mib", mib(h.usage)},
                                {"allocated_mib", mib(h.allocated)},
                                {"used_mib", mib(h.used)},
                                {"allocations", h.allocations}});
                if (h.device_local) {
                    local_usage += h.usage;
                    local_budget += h.budget;
                }
            }
            memory = Json{{"device_local_usage_mib", mib(local_usage)},
                          {"device_local_budget_mib", mib(local_budget)},
                          {"heaps", std::move(rows)}};
        }
    }

    const entt::registry& r = ctx.scene->registry();
    Json entities{{"total", count_of<scene::Transform>(r)},
                  {"meshes", count_of<scene::MeshRenderer>(r)},
                  {"skinned", count_of<scene::SkinnedMesh>(r)},
                  {"instanced_meshes", count_of<scene::InstancedMesh>(r)},
                  {"point_lights", count_of<scene::PointLight>(r)},
                  {"rigid_bodies", count_of<scene::RigidBody>(r)},
                  {"characters", count_of<scene::CharacterController>(r)},
                  {"nav_agents", count_of<scene::NavAgent>(r)},
                  {"behaviors", count_of<scene::BehaviorTree>(r)},
                  {"scripts", count_of<scene::Script>(r)},
                  {"particle_emitters", count_of<scene::ParticleEmitter>(r)},
                  {"audio_sources", count_of<scene::AudioSource>(r)}};
    std::uint64_t placements = 0;
    for (const auto [e, m] : r.view<const scene::InstancedMesh>().each()) {
        static_cast<void>(e);
        placements += m.instances.size();
    }
    entities["instance_placements"] = placements;

    return Json{{"frame", (*p)->frame_index()},
                {"counters", counters_json(**p)},
                {"gpu_memory", std::move(memory)},
                {"entities", std::move(entities)}};
}

RpcResult profile_reset(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto p = profiler_of(ctx);
    if (!p) return std::unexpected(p.error());
    (*p)->reset();
    return Json{{"reset", true}, {"frame", (*p)->frame_index()}};
}

} // namespace

void register_profile_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("profile.frame",
                      "Rolling frame profile (last 240 frames): frame CPU/GPU/wait times and "
                      "per-pass / per-system CPU (inclusive and self) and GPU avg/p95/max per "
                      "run: {sort?, kind?, limit?=30}.",
                      [ctx](const Json& p) { return profile_frame(*ctx, p); });
    server.add_method("profile.capture",
                      "Start recording {frames?=120 (1..1000), budget_ms?=16.667, top?=10}; "
                      "returns {id}. Read the summary with profile.result {id}.",
                      [ctx](const Json& p) { return profile_capture(*ctx, p); });
    server.add_method("profile.result",
                      "A capture's progress, or its summary once done: top zones by total and "
                      "p95, spikes over budget with their worst zones, per-frame totals: {id}.",
                      [ctx](const Json& p) { return profile_result(*ctx, p); });
    server.add_method("profile.counters",
                      "Per-frame counters (draws, instances, triangles, VRAM), GPU memory per "
                      "heap and entity counts by component.",
                      [ctx](const Json& p) { return profile_counters(*ctx, p); });
    server.add_method("profile.reset",
                      "Clear the rolling statistics, counters and captures.",
                      [ctx](const Json& p) { return profile_reset(*ctx, p); });
}

} // namespace engine::remote::world
