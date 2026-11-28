// project.* — the open game project and its packaging (ROADMAP P4):
// project.info, project.build (quat-pack in the background) and
// project.build_status.

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/path_confine.hpp"
#include "engine/core/utf8_path.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::array<std::string_view, 3> k_targets{"linux-x64", "windows-x64", "macos-arm64"};
constexpr std::string_view k_default_output = "dist";
constexpr std::int64_t     k_max_log_lines = 1000;

[[nodiscard]] std::string relative_to(const std::filesystem::path& root,
                                      const std::filesystem::path& p) {
    const std::filesystem::path rel = core::relative_inside(root, p);
    return core::path_to_generic_utf8(rel.empty() ? p : rel);
}

// The last `lines` lines of `log`.
[[nodiscard]] Json log_tail(const std::string& log, std::int64_t lines) {
    std::vector<std::string_view> all;
    const std::string_view text(log);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        all.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    Json out = Json::array();
    const std::size_t keep = static_cast<std::size_t>(std::max<std::int64_t>(lines, 0));
    for (std::size_t i = all.size() > keep ? all.size() - keep : 0; i < all.size(); ++i) {
        out.push_back(std::string(all[i]));
    }
    return out;
}

[[nodiscard]] Json status_json(const project::BuildStatus& s, std::int64_t log_lines,
                               const std::filesystem::path& root) {
    Json out{{"job", s.job},
             {"state", std::string(project::to_string(s.state))},
             {"progress", s.progress()},
             {"step", s.step},
             {"steps", s.steps},
             {"step_name", s.step_name},
             {"elapsed_s", s.elapsed_s}};
    if (!s.package.empty()) {
        out["package"] = relative_to(root, core::path_from_utf8(s.package));
    }
    if (!s.folder.empty()) out["folder"] = relative_to(root, core::path_from_utf8(s.folder));
    if (!s.error.empty()) out["error"] = s.error;
    out["log"] = log_tail(s.log, log_lines);
    return out;
}

RpcResult project_info(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.services.game_project) return unavailable("game projects");
    Json out{{"host_target", std::string(project::host_target())}};
    const auto game = ctx.services.game_project();
    out["open"] = game.has_value();
    if (!game) {
        out["reason"] = "no game project is open (start the engine with QUAT_GAME=<project dir>)";
        return out;
    }
    out["root"] = core::path_to_utf8(game->root);
    out["game"] = project::game_project_json(game->project);
    out["executable"] = project::executable_slug(game->project);
    if (ctx.services.project_build_status) {
        out["build"] = status_json(ctx.services.project_build_status(), 0, game->root);
    }
    return out;
}

RpcResult project_build(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    ProjectBuildParams p;
    std::string output(k_default_output);
    std::string config("shipping");
    f.string("target", p.target, 32)
        .string("output", output, 4096)
        .string("config", config, 32)
        .boolean("smoke_test", p.smoke_test);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!p.target.empty() &&
        std::find(k_targets.begin(), k_targets.end(), p.target) == k_targets.end()) {
        return bad_params("target: expected \"linux-x64\", \"windows-x64\" or \"macos-arm64\"");
    }
    auto cfg = project::parse_package_config(config);
    if (!cfg) return bad_params("config: " + cfg.error().message);
    p.config = *cfg;
    if (!ctx.services.game_project || !ctx.services.project_build) {
        return unavailable("game packaging");
    }
    const auto game = ctx.services.game_project();
    if (!game) {
        return rpc_fail(rpc_code::failed,
                        "no game project is open; start the engine with QUAT_GAME=<project dir>");
    }
    auto out = core::confine_to_root(game->root, output);
    if (!out) return bad_params("output: " + out.error().message);
    // Packages must not land in (or swallow) what the game ships.
    if (auto ok = project::check_output_folder(game->project, game->root, *out); !ok) {
        return bad_params(ok.error().message);
    }
    p.output = *out;
    auto job = ctx.services.project_build(p);
    if (!job) return rpc_fail(rpc_code::failed, job.error().message);
    return Json{{"job", *job},
                {"state", "running"},
                {"target", p.target.empty() ? std::string(project::host_target()) : p.target},
                {"config", std::string(project::to_string(p.config))},
                {"output", core::path_to_generic_utf8(
                               core::path_from_utf8(output).lexically_normal())}};
}

RpcResult project_build_status(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::int64_t log_lines = 40;
    f.integer("log_lines", log_lines, 0, k_max_log_lines);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.services.project_build_status) return unavailable("game packaging");
    std::filesystem::path root;
    if (ctx.services.game_project) {
        if (const auto game = ctx.services.game_project()) root = game->root;
    }
    return status_json(ctx.services.project_build_status(), log_lines, root);
}

} // namespace

void register_project_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("project.info",
                      "The open game project: {open, root, game (game.json), executable, "
                      "host_target, build}.",
                      [ctx](const Json& p) { return project_info(*ctx, p); });
    server.add_method("project.build",
                      "Package the game for the host OS in the background: {target?, output?, "
                      "config?, smoke_test?} -> {job, state, target, config, output}.",
                      [ctx](const Json& p) { return project_build(*ctx, p); });
    server.add_method("project.build_status",
                      "The packaging job: {log_lines?} -> {job, state, progress, step, steps, "
                      "step_name, package?, folder?, error?, elapsed_s, log}.",
                      [ctx](const Json& p) { return project_build_status(*ctx, p); });
}

} // namespace engine::remote::world
