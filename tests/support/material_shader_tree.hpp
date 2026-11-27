#pragma once

// A scratch project for material-shader tests (ROADMAP A6): an empty
// assets/shaders/materials directory, and a cooked/shaders directory holding
// what a material compile links against — the engine's library modules and
// the built-in GBuffer program (whose layout a material must fit). The
// prerequisites are compiled from the real shader sources once per test run
// (slangc) and copied into each tree. Material sources for tests live in
// tests/data/materials/.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "engine/rhi/material_shader_registry.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "support/temp_dir.hpp"

namespace quat::test {

namespace material_tree_detail {
namespace fs = std::filesystem;

inline fs::path repo_root() {
    return fs::path(QUAT_COOKED_SHADER_DIR).parent_path().parent_path();
}

// Compiles the prerequisites once; empty path (with `error`) on failure.
inline const fs::path& prerequisites(std::string& error) {
    static std::unique_ptr<TempDir> dir;
    static std::string              failure;
    if (!dir) {
        dir = std::make_unique<TempDir>("material_prereqs");
        const fs::path shaders = repo_root() / "assets" / "shaders";
        auto compiler = engine::rhi::ShaderCompiler::create({{}, shaders, {}, dir->path(), {}});
        if (!compiler) {
            failure = compiler.error().message;
        } else {
            const fs::path lib = shaders / "lib";
            for (const char* source :
                 {"lib/material.slang", "lib/instancing.slang", "lib/pbr.slang",
                  "lib/gbuffer_common.slang", "lib/material_pass_gbuffer.slang",
                  "gbuffer.slang"}) {
                const auto r = compiler->compile(shaders / source);
                if (!r.ok) {
                    failure = std::string(source) + ": " + r.diagnostics;
                    break;
                }
            }
        }
    }
    error = failure;
    static const fs::path none;
    return failure.empty() ? dir->path() : none;
}
} // namespace material_tree_detail

inline std::filesystem::path repo_shader_dir() {
    return material_tree_detail::repo_root() / "assets" / "shaders";
}

inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// tests/data/materials/<file>
inline std::string test_material(std::string_view file) {
    return read_text_file(material_tree_detail::repo_root() / "tests" / "data" / "materials" /
                          std::string(file));
}

class MaterialShaderTree {
public:
    explicit MaterialShaderTree(const std::string& prefix) : root_(prefix) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(materials, ec);
        fs::create_directories(cooked, ec);
        const fs::path& pre = material_tree_detail::prerequisites(error);
        if (!error.empty()) return;
        for (const auto& entry : fs::directory_iterator(pre, ec)) {
            if (entry.is_regular_file()) {
                fs::copy_file(entry.path(), cooked / entry.path().filename(),
                              fs::copy_options::overwrite_existing, ec);
            }
        }
    }

    [[nodiscard]] bool ready() const { return error.empty(); }

    [[nodiscard]] engine::rhi::ShaderHotReload::Config reload_config() const {
        engine::rhi::ShaderHotReload::Config c;
        c.source_dir = shaders;
        c.module_dir = repo_shader_dir() / "lib";
        c.material_dir = materials;
        c.output_dir = cooked;
        c.debounce = std::chrono::milliseconds(0);
        c.poll_interval = std::chrono::milliseconds(0);
        c.async = false;
        return c;
    }

    [[nodiscard]] engine::rhi::MaterialShaderRegistry::Config registry_config() const {
        engine::rhi::MaterialShaderRegistry::Config c;
        c.material_dir = materials;
        c.module_dir = repo_shader_dir() / "lib";
        c.template_path = repo_shader_dir() / "templates" / "material.slang";
        c.builtin_path = repo_shader_dir() / "lib" / "material.slang";
        c.cooked_dir = cooked;
        c.display_root = project;
        return c;
    }

private:
    TempDir root_;

public:
    std::filesystem::path project = root_.path();
    std::filesystem::path shaders = project / "assets" / "shaders";
    std::filesystem::path materials = shaders / "materials";
    std::filesystem::path cooked = project / "cooked" / "shaders";
    std::string           error;
};

} // namespace quat::test
