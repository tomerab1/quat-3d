// debug.log, debug.stats and the simulation clock (debug.pause / resume /
// step / time_scale) — ROADMAP A5. Queries against the world (watch,
// raycast, nav_path) are in debug_query_commands.cpp.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/log.hpp"
#include "engine/core/sim_clock.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

Json sim_clock_json(const core::SimClock& clock) {
    const core::SimClockState s = clock.state();
    return Json{{"paused", s.paused},
                {"time_scale", s.time_scale},
                {"pending_steps", s.pending_steps},
                {"steps_run", s.steps_run},
                {"sim_frames", s.sim_frames},
                {"sim_time_s", s.sim_time_s}};
}

namespace {

constexpr int k_max_log_limit = 1000;
constexpr std::size_t k_max_filter = 256;
constexpr int k_max_step_frames = 10'000;
// Outstanding debug.step waits (each parks a responder until its steps run).
constexpr std::size_t k_max_step_waits = 64;

[[nodiscard]] Json log_line_json(const core::LogLine& l) {
    return Json{{"seq", l.seq},
                {"t", l.time_s},
                {"level", std::string(core::to_string(l.level))},
                {"category", l.category},
                {"message", l.message}};
}

RpcResult debug_log(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    core::LogQuery q;
    std::int64_t since = -1;
    std::string level;
    int limit = 100;
    f.integer("since_seq", since, 0, std::numeric_limits<std::int64_t>::max())
        .string("level", level, 16)
        .string("category", q.category, 64)
        .string("contains", q.contains, k_max_filter)
        .integer("limit", limit, 1, k_max_log_limit);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!level.empty()) {
        const auto parsed = core::parse_log_level(level);
        if (!parsed) {
            return bad_params("level: expected one of \"trace\", \"debug\", \"info\", \"warn\", "
                              "\"error\" (the minimum level to return)");
        }
        q.min_level = *parsed;
    }
    if (ctx.services.log_ring == nullptr) return unavailable("the engine log");
    if (since >= 0) q.since_seq = static_cast<std::uint64_t>(since);
    q.limit = static_cast<std::size_t>(limit);

    const core::LogRing& ring = *ctx.services.log_ring;
    const core::LogQueryResult r = ring.query(q);
    Json lines = Json::array();
    for (const core::LogLine& l : r.lines) lines.push_back(log_line_json(l));
    return Json{{"lines", std::move(lines)},
                {"cursor", r.cursor},
                {"more", r.more},
                {"missed", r.missed},
                {"oldest_seq", ring.first_seq()},
                {"capacity", ring.capacity()}};
}

RpcResult debug_stats(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    const entt::registry& r = ctx.scene->registry();

    Json out = Json::object();
    if (ctx.services.frame_stats) {
        const FrameStats fs = ctx.services.frame_stats();
        out["frame"] = fs.frame;
        out["frame_time_ms"] = {{"last", fs.last_ms},
                                {"avg", fs.avg_ms},
                                {"worst", fs.worst_ms},
                                {"samples", fs.samples}};
        out["fps"] = fs.avg_ms > 0.0F ? 1000.0F / fs.avg_ms : 0.0F;
        out["draws"] = fs.draws ? Json(*fs.draws) : Json(nullptr);
        out["memory"] = fs.resident_bytes ? Json{{"resident_bytes", *fs.resident_bytes}}
                                          : Json(nullptr);
    } else {
        out["frame"] = nullptr;
        out["frame_time_ms"] = nullptr;
        out["fps"] = nullptr;
        out["draws"] = nullptr;
        out["memory"] = nullptr;
    }
    out["entities"] = r.view<const scene::Transform>().size();

    const physics::PhysicsWorld* world =
        ctx.services.physics_world ? ctx.services.physics_world() : nullptr;
    out["physics"] = world != nullptr ? Json{{"bodies", world->body_count()},
                                             {"active_bodies", world->active_body_count()},
                                             {"characters", world->character_count()}}
                                      : Json(nullptr);

    std::size_t agents = 0;
    std::size_t moving = 0;
    std::size_t failed = 0;
    for (const auto [e, a] : r.view<const scene::NavAgent>().each()) {
        (void)e;
        ++agents;
        moving += a.active && !a.paused && !a.arrived ? 1U : 0U;
        failed += a.failed ? 1U : 0U;
    }
    const nav::NavMesh* mesh = ctx.services.navmesh ? ctx.services.navmesh() : nullptr;
    out["nav"] = {{"agents", agents},
                  {"moving", moving},
                  {"failed", failed},
                  {"navmesh_polygons",
                   mesh != nullptr && mesh->valid() ? Json(mesh->polygon_count()) : Json(nullptr)}};

    out["sim"] = ctx.services.sim_clock != nullptr ? sim_clock_json(*ctx.services.sim_clock)
                                                   : Json(nullptr);

    if (ctx.services.log_ring != nullptr) {
        // Counted on push: no copy of the ring (up to its capacity of lines).
        const core::LogCounts c = ctx.services.log_ring->counts();
        out["log"] = {
            {"last_seq", ctx.services.log_ring->last_seq()},
            {"warnings", c.warnings},
            {"errors", c.errors},
            {"last_error_seq", c.last_error_seq == 0 ? Json(nullptr) : Json(c.last_error_seq)}};
    } else {
        out["log"] = nullptr;
    }
    return out;
}

[[nodiscard]] std::expected<core::SimClock*, RpcError> clock_of(Context& ctx) {
    if (ctx.services.sim_clock == nullptr) return unavailable("the simulation clock");
    return ctx.services.sim_clock;
}

RpcResult debug_pause(Context& ctx, const Json& params, bool pause) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto clock = clock_of(ctx);
    if (!clock) return std::unexpected(clock.error());
    if (pause) {
        (*clock)->pause();
    } else {
        (*clock)->resume();
    }
    return sim_clock_json(**clock);
}

RpcResult debug_time_scale(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    float scale = -1.0F;
    f.number("scale", scale, core::SimClock::min_time_scale, core::SimClock::max_time_scale);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (scale < 0.0F) {
        return bad_params("scale: required (simulation speed multiplier, 1 = real time, 0.5 = half "
                          "speed; range [0.01, 10])");
    }
    auto clock = clock_of(ctx);
    if (!clock) return std::unexpected(clock.error());
    (*clock)->set_time_scale(scale);
    return sim_clock_json(**clock);
}

void debug_step(const ContextPtr& ctx, const Json& params, Responder responder) {
    core::JsonFields f(params, "");
    int frames = 1;
    float dt = core::SimClock::default_step_dt;
    f.integer("frames", frames, 1, k_max_step_frames)
        .number("dt", dt, 1e-4F, core::SimClock::max_step_dt);
    if (auto ok = finish(f); !ok) {
        responder.reply(std::unexpected(ok.error()));
        return;
    }
    auto clock = clock_of(*ctx);
    if (!clock) {
        responder.reply(std::unexpected(clock.error()));
        return;
    }
    const bool waits = static_cast<bool>(ctx->services.when_stepped);
    if (waits && ctx->step_waits >= k_max_step_waits) {
        responder.reply(rpc_fail(rpc_code::failed,
                                 "busy: " + std::to_string(k_max_step_waits) +
                                     " debug.step requests are already waiting for their steps; "
                                     "retry once one replies, or debug.resume to drop them"));
        return;
    }
    const auto target = (*clock)->step(static_cast<std::uint32_t>(frames), dt);
    if (!target) {
        responder.reply(rpc_fail(rpc_code::failed, "debug.step: " + target.error().message));
        return;
    }
    if (!waits) {
        responder.reply(sim_clock_json(**clock));
        return;
    }
    // std::function needs a copyable callable; the Responder is move-only.
    auto shared = std::make_shared<Responder>(std::move(responder));
    ++ctx->step_waits;
    ctx->services.when_stepped(*target, [ctx, shared] {
        --ctx->step_waits;
        if (ctx->services.sim_clock != nullptr) {
            shared->reply(sim_clock_json(*ctx->services.sim_clock));
        } else {
            shared->reply(unavailable("the simulation clock"));
        }
    });
}

} // namespace

void register_debug_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("debug.log",
                      "Recent engine log lines: {since_seq?, level?, category?, contains?, "
                      "limit?=100} -> {lines:[{seq, t, level, category, message}], cursor, more, "
                      "missed}. Pass cursor back as since_seq to tail.",
                      [ctx](const Json& p) { return debug_log(*ctx, p); });
    server.add_method("debug.stats",
                      "Frame time (last/avg/worst), fps, entity/draw counts, physics bodies, nav "
                      "agents, memory, simulation clock and log warning/error counts.",
                      [ctx](const Json& p) { return debug_stats(*ctx, p); });
    server.add_method("debug.pause", "Pause the simulation (rendering continues): clock state.",
                      [ctx](const Json& p) { return debug_pause(*ctx, p, true); });
    server.add_method("debug.resume",
                      "Resume real-time simulation (drops pending steps): clock state.",
                      [ctx](const Json& p) { return debug_pause(*ctx, p, false); });
    server.add_method("debug.time_scale",
                      "Set the simulation speed: {scale} in [0.01, 10] -> clock state.",
                      [ctx](const Json& p) { return debug_time_scale(*ctx, p); });
    server.add_deferred_method(
        "debug.step",
        "Pause and advance the simulation {frames?=1} frames of {dt?=1/60} s each; replies with "
        "the clock state once they have run.",
        [ctx](const Json& p, Responder r) { debug_step(ctx, p, std::move(r)); });
}

} // namespace engine::remote::world
