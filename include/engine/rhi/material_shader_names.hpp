#pragma once

// Naming and file layout of authored material shaders (ROADMAP A6), shared by
// the compiler service, the registry behind the shader.* remote methods, the
// scene codec and the renderer. Header-only, no Vulkan.
//
//   assets/shaders/materials/<name>.slang          the module (implements IMaterial)
//   assets/shaders/lib/<driver>.slang              one pass driver per variant
//   cooked/shaders/materials/<name>.<variant>.spv  + .json reflection
//
// A material shader is compiled once per variant: linked against the
// variant's driver module, which supplies the pass entry points and leaves
// `extern struct SurfaceMaterial : IMaterial` for the material to export.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace engine::rhi::material_shaders {

// Directory of material sources under the shader source dir, and of their
// cooked variants under the cooked shader dir.
inline constexpr std::string_view subdir = "materials";

// Name reported for the built-in material (glTF metallic-roughness PBR); an
// empty selection means the same.
inline constexpr std::string_view builtin_name = "standard";

inline constexpr std::size_t max_name_length = 48;

struct Variant {
    std::string_view name;          // output suffix: <material>.<name>.spv
    std::string_view driver_module; // library module in the module dir
    // The pass's built-in program (cooked/shaders/<builtin_program>.spv): a
    // material's program must fit its pipeline layout.
    std::string_view builtin_program;
};

// Every variant a material shader is compiled into.
inline constexpr std::array<Variant, 1> variants{{
    {"gbuffer", "material_pass_gbuffer", "gbuffer"},
}};

// A material shader name: a lowercase identifier ([a-z][a-z0-9_]*, at most
// max_name_length characters) — it is the file stem and the Slang module name.
[[nodiscard]] constexpr bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > max_name_length) return false;
    if (name.front() < 'a' || name.front() > 'z') return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// <cooked_dir>/materials/<name>.<variant>.spv
[[nodiscard]] inline std::string cooked_spv_path(std::string_view cooked_dir,
                                                 std::string_view name,
                                                 std::string_view variant) {
    std::string out(cooked_dir);
    out += '/';
    out += subdir;
    out += '/';
    out += name;
    out += '.';
    out += variant;
    out += ".spv";
    return out;
}

} // namespace engine::rhi::material_shaders
