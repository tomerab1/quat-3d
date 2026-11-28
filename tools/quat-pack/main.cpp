// quat-pack: packages a game made with the engine for the host OS (ROADMAP
// P4). The work happens in engine_pack (include/engine/pack/pack_cli.hpp);
// this entry point only gathers the arguments (as UTF-8) and the defaults.
// See docs/game-packaging.md.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/child_process.hpp"
#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/pack/pack_cli.hpp"

namespace {

int run(const std::vector<std::string>& args) {
    const std::string_view usage = engine::pack::pack_usage();
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        std::fwrite(usage.data(), 1, usage.size(), stdout);
        return 0;
    }
    auto options = engine::pack::parse_pack_args(args);
    if (!options) {
        std::fprintf(stderr, "quat-pack: %s\n%.*s", options.error().message.c_str(),
                     static_cast<int>(usage.size()), usage.data());
        return 2;
    }

    engine::pack::PackDefaults defaults;
#if defined(QUAT_VERSION)
    defaults.engine_version = std::string("quat-3d ") + QUAT_VERSION;
#endif
#if defined(QUAT_BUILD_TREE_SHADER_DIR)
    defaults.shader_dir = engine::core::path_from_utf8(QUAT_BUILD_TREE_SHADER_DIR);
#endif
#if defined(QUAT_BUILD_TREE_ROOT)
    defaults.engine_root = engine::core::path_from_utf8(QUAT_BUILD_TREE_ROOT);
#endif
    if (auto root = engine::core::env_path("QUAT_ENGINE_ROOT")) defaults.engine_root = root;
    if (auto shaders = engine::core::env_path("QUAT_SHADER_DIR")) defaults.shader_dir = shaders;
    if (auto self = engine::core::current_executable_path()) defaults.tool_dir = self->parent_path();
    // Installed in an engine SDK (<sdk>/bin): engine root and shaders from
    // <sdk>/share/quat3d, unless the build tree or the environment set them.
    engine::pack::apply_installed_sdk_defaults(defaults);

    return engine::pack::run_pack(*options, defaults, [](std::string_view text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    });
}

} // namespace

#if defined(_WIN32)
// Wide arguments: the narrow argv would be in the ANSI code page.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(engine::core::path_to_utf8(std::filesystem::path(argv[i])));
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#endif
