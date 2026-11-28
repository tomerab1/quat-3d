#include "engine/pack/pack_cli.hpp"

#include <chrono>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/core/child_process.hpp"
#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/pack/dependencies.hpp"
#include "engine/pack/stage.hpp"
#include "engine/project/build_job.hpp"
#include "engine/project/game_project.hpp"

namespace engine::pack {

namespace {

namespace fs = std::filesystem;
using core::path_from_utf8;
using core::path_to_generic_utf8;
using core::path_to_utf8;

constexpr std::string_view k_usage =
    "usage: quat-pack <project dir | game.json> --player <quat-player> --out <dir>\n"
    "                 [--config shipping|development] [--build-id <id>]\n"
    "                 [--cook auto|off] [--quat-cook <exe>] [--shaders <dir>]\n"
    "                 [--engine-root <dir>] [--stage-only] [--smoke-test]\n";

constexpr std::chrono::minutes k_layout_timeout{30};
constexpr std::chrono::minutes k_smoke_timeout{10};

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::string archive_extension(std::string_view target) {
    return target == "linux-x64" ? ".tar.gz" : ".zip";
}

// Bash for the layout scripts. On Windows: Git Bash, never System32's WSL
// launcher.
[[nodiscard]] std::optional<fs::path> find_bash() {
    if (auto env = core::env_path("QUAT_BASH")) return env;
#if defined(_WIN32)
    for (const char* var : {"ProgramW6432", "ProgramFiles"}) {
        if (auto pf = core::env_path(var)) {
            const fs::path git = *pf / "Git" / "bin" / "bash.exe";
            std::error_code ec;
            if (fs::is_regular_file(git, ec)) return git;
        }
    }
    if (auto bash = core::find_on_path("bash")) {
        std::string lower = path_to_generic_utf8(*bash);
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (lower.find("/system32/") == std::string::npos) return bash;
    }
    return std::nullopt;
#else
    std::error_code ec;
    if (fs::is_regular_file("/bin/bash", ec)) return fs::path("/bin/bash");
    return core::find_on_path("bash");
#endif
}

constexpr std::string_view k_shipping_player = "quat-player-shipping";

} // namespace

std::expected<fs::path, core::Error> player_for_config(const fs::path& player,
                                                       project::PackageConfig config) {
    const bool is_shipping = path_to_utf8(player.stem()) == k_shipping_player;
    if (config == project::PackageConfig::development) {
        if (is_shipping) {
            return fail("--player: '" + path_to_utf8(player) +
                        "' is the shipping player, which has no remote control; a development "
                        "package needs quat-player");
        }
        return player;
    }
    if (is_shipping) return player;
    fs::path shipping = player.parent_path() / std::string(k_shipping_player);
    shipping += player.extension(); // ".exe" on Windows
    std::error_code ec;
    if (!fs::is_regular_file(shipping, ec)) {
        return fail("--config shipping packages quat-player-shipping (remote control compiled "
                    "out), but there is none at '" +
                    path_to_utf8(shipping) + "'; build the quat-player-shipping target");
    }
    return shipping;
}

namespace {

[[nodiscard]] std::optional<fs::path> find_quat_cook(const PackOptions& o, const PackDefaults& d) {
    if (o.cook == CookMode::off) return std::nullopt;
    if (o.quat_cook) return o.quat_cook;
#if defined(_WIN32)
    constexpr const char* k_name = "quat-cook.exe";
#else
    constexpr const char* k_name = "quat-cook";
#endif
    if (d.tool_dir) {
        const fs::path sibling = *d.tool_dir / k_name;
        std::error_code ec;
        if (fs::is_regular_file(sibling, ec)) return sibling;
    }
    return core::find_on_path("quat-cook");
}

class Driver {
public:
    Driver(const PackOptions& o, const PackDefaults& d,
           const std::function<void(std::string_view)>& out)
        : o_(o), d_(d), out_(out) {}

    int run() {
        steps_ = 3 + (o_.stage_only ? 0 : 1) + (o_.smoke_test && !o_.stage_only ? 1 : 0);
        if (auto ok = run_steps(); !ok) {
            line("[pack] error: " + ok.error().message);
            return 1;
        }
        return 0;
    }

private:
    void line(const std::string& text) const {
        out_(text);
        out_("\n");
    }
    void step(const std::string& what) {
        ++step_;
        line("[pack] step " + std::to_string(step_) + "/" + std::to_string(steps_) + " " + what);
    }

    std::expected<void, core::Error> run_steps() {
        // ---- 1. Project --------------------------------------------------
        step("validate game project");
        fs::path root = o_.project;
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) root = root.parent_path();
        root = fs::absolute(root, ec).lexically_normal();
        if (!root.has_filename()) root = root.parent_path(); // "dir/"
        auto project = project::load_game_project(root);
        if (!project) return std::unexpected(project.error());
        const std::string slug = project::executable_slug(*project);
        const std::string target(project::host_target());
        // --out is relative to the working directory, not the project.
        if (auto ok = project::check_output_folder(*project, root, fs::absolute(o_.out, ec)); !ok) {
            return fail("--out: " + ok.error().message.substr(std::string("output: ").size()));
        }
        line("game: " + project->name + " " + project->version + " (" + project->id + ") from " +
             path_to_utf8(root));

        fs::path player = o_.player;
        if (!o_.stage_only) {
#if defined(_WIN32)
            // "build/quat-player" names build/quat-player.exe, as in a shell.
            if (!fs::is_regular_file(player, ec) && !player.has_extension()) player += ".exe";
#endif
            if (!fs::is_regular_file(player, ec)) {
                return fail("--player: no quat-player at '" + path_to_utf8(o_.player) + "'");
            }
            player = fs::absolute(player, ec);
            auto chosen = player_for_config(player, o_.config);
            if (!chosen) return std::unexpected(chosen.error());
            player = *chosen;
            line("player: " + path_to_utf8(player));
        }
        const fs::path shaders = o_.shaders ? *o_.shaders
                                 : d_.shader_dir ? *d_.shader_dir
                                 : player.parent_path() / "cooked" / "shaders";

        // ---- 2. Files ----------------------------------------------------
        step("collect game files");
        auto files = collect_game_files(root, *project);
        if (!files) return std::unexpected(files.error());
        for (const std::string& w : files->warnings) line("[pack] warning: " + w);
        for (const PackFile& f : files->files) {
            line("  " + std::string(project::to_string(f.kind)) + "  " + f.path + "  (" + f.reason +
                 ")");
        }
        line(std::to_string(files->files.size()) + " game files");

        // ---- 3. Stage ----------------------------------------------------
        const fs::path out = fs::absolute(o_.out, ec).lexically_normal();
        const std::string version =
            project->version + (o_.build_id.empty() ? "" : "-" + o_.build_id);
        const fs::path work = out / ".quat-pack" / (slug + "-" + target);
        fs::remove_all(work, ec); // ours: a fixed name under the output folder
        if (ec) return fail("cannot clear '" + path_to_utf8(work) + "': " + ec.message());
        const auto cook = find_quat_cook(o_, d_);
        step(cook ? "stage data (cooking models with " + path_to_utf8(*cook) + ")"
                  : "stage data");
        if (!cook && o_.cook != CookMode::off) line("quat-cook not found: models ship as authored");
        StageOptions so;
        so.project_root = root;
        so.project = *project;
        so.files = std::move(*files);
        so.data_dir = work / "data";
        so.shader_dir = shaders;
        so.quat_cook = cook;
        so.config = o_.config;
        so.target = target;
        so.build_id = o_.build_id;
        so.engine_version = d_.engine_version;
        so.log = [this](std::string_view l) { line("  " + std::string(l)); };
        auto manifest = stage_game_data(so);
        if (!manifest) return std::unexpected(manifest.error());
        std::uint64_t bytes = 0;
        for (const project::ManifestFile& f : manifest->files) bytes += f.bytes;
        line("staged " + std::to_string(manifest->files.size()) + " files, " +
             std::to_string(bytes) + " bytes (" + std::string(project::to_string(o_.config)) +
             ")");
        if (o_.stage_only) {
            line("[pack] folder " + path_to_generic_utf8(so.data_dir));
            return {};
        }

        // ---- 4. Layout ---------------------------------------------------
        step("lay out " + target + " package");
        auto engine_root = o_.engine_root ? o_.engine_root : d_.engine_root;
        if (engine_root) engine_root = fs::absolute(*engine_root, ec).lexically_normal();
        if (!engine_root) {
            return fail("no engine root (scripts/package-release.sh): pass --engine-root or set "
                        "QUAT_ENGINE_ROOT");
        }
        const auto bash = find_bash();
        if (!bash) {
            return fail("bash not found (Windows: install Git for Windows, or set QUAT_BASH)");
        }
        const fs::path player_dir = player.parent_path();
        std::vector<std::string> args{
            path_to_generic_utf8(*engine_root / "scripts" / "package-release.sh"),
            path_to_generic_utf8(player_dir),
            path_to_generic_utf8(out),
            version,
            "--exe",
            path_to_utf8(player.stem()),
            "--package-exe",
            slug,
            "--data",
            path_to_generic_utf8(so.data_dir),
            "--name",
            slug,
            "--app-name",
            project->name,
            "--display-name",
            project::window_title(*project),
            "--bundle-id",
            project->id,
            "--readme-dir",
            path_to_generic_utf8(*engine_root / "packaging" / "game")};
        if (!project->icon.empty()) {
            args.emplace_back("--icon");
            args.push_back(path_to_generic_utf8(so.data_dir / path_from_utf8(project->icon)));
        }
        if (auto ok = run_script(*bash, args, k_layout_timeout, "package-release.sh"); !ok) {
            return ok;
        }
        const std::string pkg = slug + "-" + version + "-" + target;
        const fs::path folder = out / pkg;
        const fs::path archive = out / (pkg + archive_extension(target));
        if (!fs::is_regular_file(archive, ec)) {
            return fail("package-release.sh did not write '" + path_to_utf8(archive) + "'");
        }
        // '/' separators everywhere: shells (CI) and BuildJob read these.
        line("[pack] folder " + path_to_generic_utf8(folder));
        line("[pack] package " + path_to_generic_utf8(archive));

        // ---- 5. Smoke test -----------------------------------------------
        if (o_.smoke_test) {
            step("smoke test");
            const std::vector<std::string> smoke{
                path_to_generic_utf8(*engine_root / "scripts" / "smoke-test-package.sh"),
                path_to_generic_utf8(archive),
                path_to_generic_utf8(work / "smoke"),
                "--exe",
                slug,
                "--expect",
                "[game] start scene loaded"};
            if (auto ok = run_script(*bash, smoke, k_smoke_timeout, "smoke-test-package.sh"); !ok) {
                return ok;
            }
        }
        return {};
    }

    std::expected<void, core::Error> run_script(const fs::path& bash,
                                                const std::vector<std::string>& args,
                                                std::chrono::minutes timeout,
                                                std::string_view what) const {
        core::ProcessOptions opts;
        opts.timeout = timeout;
        opts.on_output = [this](std::string_view chunk) { out_(chunk); };
        auto r = core::run_process(bash, std::span<const std::string>(args), opts);
        if (!r) return fail(std::string(what) + ": " + r.error().message);
        if (!r->succeeded()) {
            return fail(std::string(what) + " failed (exit code " + std::to_string(r->exit_code) +
                        "); see the log above");
        }
        return {};
    }

    const PackOptions&                            o_;
    const PackDefaults&                           d_;
    const std::function<void(std::string_view)>& out_;
    int                                           steps_ = 0;
    int                                           step_ = 0;
};

} // namespace

std::string_view pack_usage() { return k_usage; }

std::expected<void, core::Error> validate_build_id(std::string_view id) {
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (id.empty() || id.size() > 64 || !alnum(id.front())) {
        return fail("--build-id: expected 1-64 characters of [A-Za-z0-9.-], starting with a "
                    "letter or digit");
    }
    for (const char c : id) {
        if (!alnum(c) && c != '.' && c != '-') {
            return fail("--build-id: only letters, digits, '.' and '-' are allowed");
        }
    }
    return {};
}

std::expected<PackOptions, core::Error> parse_pack_args(std::span<const std::string> args) {
    PackOptions o;
    bool have_project = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const auto value = [&]() -> std::expected<std::string, core::Error> {
            if (i + 1 >= args.size()) return fail(a + ": missing value");
            return args[++i];
        };
        if (a == "--stage-only") {
            o.stage_only = true;
        } else if (a == "--smoke-test") {
            o.smoke_test = true;
        } else if (a == "--player" || a == "--out" || a == "--config" || a == "--build-id" ||
                   a == "--cook" || a == "--quat-cook" || a == "--shaders" ||
                   a == "--engine-root") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            if (a == "--player") {
                o.player = path_from_utf8(*v);
            } else if (a == "--out") {
                o.out = path_from_utf8(*v);
            } else if (a == "--config") {
                auto c = project::parse_package_config(*v);
                if (!c) return fail("--config: " + c.error().message);
                o.config = *c;
            } else if (a == "--build-id") {
                if (auto ok = validate_build_id(*v); !ok) return std::unexpected(ok.error());
                o.build_id = *v;
            } else if (a == "--cook") {
                if (*v != "auto" && *v != "off") return fail("--cook: expected auto or off");
                o.cook = *v == "off" ? CookMode::off : CookMode::automatic;
            } else if (a == "--quat-cook") {
                o.quat_cook = path_from_utf8(*v);
            } else if (a == "--shaders") {
                o.shaders = path_from_utf8(*v);
            } else {
                o.engine_root = path_from_utf8(*v);
            }
        } else if (a.starts_with("-")) {
            return fail("unknown option '" + a + "'");
        } else if (have_project) {
            return fail("more than one project given ('" + a + "')");
        } else {
            o.project = path_from_utf8(a);
            have_project = true;
        }
    }
    if (!have_project) return fail("no game project given");
    if (o.out.empty()) return fail("--out is required");
    if (!o.stage_only && o.player.empty()) return fail("--player is required (or --stage-only)");
    if (o.smoke_test && o.stage_only) return fail("--smoke-test needs a package (drop --stage-only)");
    return o;
}

int run_pack(const PackOptions& options, const PackDefaults& defaults,
             const std::function<void(std::string_view)>& out) {
    return Driver(options, defaults, out).run();
}

} // namespace engine::pack
