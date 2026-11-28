#include "app/build_service.hpp"

#include <optional>
#include <string_view>
#include <utility>

#include "engine/core/child_process.hpp"
#include "engine/core/env.hpp"

namespace quat::app {

namespace {

namespace fs = std::filesystem;

// `env` if set, else `name` next to the running executable.
[[nodiscard]] fs::path sibling_tool(const char* env, std::string_view name) {
    if (auto p = engine::core::env_path(env)) return *p;
#if defined(_WIN32)
    const std::string file = std::string(name) + ".exe";
#else
    const std::string file(name);
#endif
    if (auto self = engine::core::current_executable_path()) return self->parent_path() / file;
    return fs::path(file);
}

} // namespace

std::expected<std::uint64_t, engine::core::Error>
start_game_build(engine::project::BuildJob& job, const fs::path& project_root,
                 const GameBuildSettings& settings) {
    const std::string_view host = engine::project::host_target();
    if (!settings.target.empty() && settings.target != host) {
        return std::unexpected(engine::core::Error{
            "cannot package for " + settings.target + " on " + std::string(host) +
            ": a package is built on its own OS (it needs that OS's player and tools; CI "
            "packages all three)"});
    }
    engine::project::BuildRequest request;
    request.quat_pack = sibling_tool("QUAT_PACK", "quat-pack");
    request.player = sibling_tool("QUAT_PLAYER", "quat-player");
    request.project = project_root;
    request.output = settings.output;
    request.config = settings.config;
    request.smoke_test = settings.smoke_test;
    return job.start(std::move(request));
}

} // namespace quat::app
