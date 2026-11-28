// quat-cook: offline glTF cooker (KTX2/Basis textures, meshoptimizer meshes).
// Usage is documented in docs/DEVELOPMENT.md ("Cooking assets").

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "engine/cook/gltf_cooker.hpp"
#include "engine/cook/terrain_layer.hpp"

namespace {

constexpr std::string_view k_usage =
    "usage: quat-cook <input.gltf|glb> <output.gltf|glb> [options]\n"
    "  --color-codec etc1s|uastc    codec for non-normal textures (default etc1s)\n"
    "  --normal-codec etc1s|uastc   codec for normal maps (default uastc)\n"
    "  --normal-layout xy|rgb       normal map storage (default xy -> BC5)\n"
    "  --etc1s-quality N            1..255 (default 128)\n"
    "  --uastc-level N              0..4 (default 2)\n"
    "  --uastc-rdo Q                UASTC RDO quality scalar, 0 = off (default 1.0)\n"
    "  --zstd N                     UASTC Zstd level, 0 = off (default 18)\n"
    "  --no-mips  --no-textures\n"
    "  --no-optimize  --no-quantize  --no-compress\n"
    "  --position-bits N            1..24 (default 16)\n"
    "  --normal-bits 8|16           (default 8)\n"
    "  --threads N                  encoder threads (default: all cores)\n"
    "  --simplify R                 keep about R (0..1) of each mesh's triangles (default 1)\n"
    "  --simplify-error E           simplifier error bound, relative to the model size\n"
    "                               (default 0.01)\n"
    "  --blend-to-mask C            BLEND materials become MASK with alpha cutoff C\n"
    "\n"
    "usage: quat-cook --terrain-layer <output-dir> --albedo <img> --normal <img>\n"
    "                 [--arm <img> | --roughness <img> --ao <img>] [--height <img>]\n"
    "                 [--normal-dx] [--size N] [--threads N]\n"
    "  terrain layer texture set: albedo.ktx2, normal.ktx2, packed.ktx2 (N x N,\n"
    "  power of two, default 1024); --normal-dx for DirectX-convention normal maps\n";

[[nodiscard]] bool parse_codec(std::string_view s, engine::cook::BasisCodec& out) {
    if (s == "etc1s") out = engine::cook::BasisCodec::etc1s;
    else if (s == "uastc") out = engine::cook::BasisCodec::uastc;
    else return false;
    return true;
}

[[nodiscard]] bool parse_number(const char* s, double& out) {
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end != s && *end == '\0';
}

[[nodiscard]] int usage_error(std::string_view what) {
    std::fprintf(stderr, "quat-cook: %.*s\n%.*s", static_cast<int>(what.size()), what.data(),
                 static_cast<int>(k_usage.size()), k_usage.data());
    return 2;
}

// quat-cook --terrain-layer <out-dir> ... (R26.2).
[[nodiscard]] int cook_terrain_layer_cli(int argc, char** argv) {
    if (argc < 3) return usage_error("missing output directory");
    engine::cook::TerrainLayerSources src;
    engine::cook::TerrainLayerCookOptions o;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--normal-dx") {
            src.normal_opengl = false;
            continue;
        }
        if (i + 1 >= argc) return usage_error("missing value: " + std::string(arg));
        const char* value = argv[++i];
        double n = 0;
        if (arg == "--albedo") src.albedo = value;
        else if (arg == "--normal") src.normal = value;
        else if (arg == "--arm") src.arm = value;
        else if (arg == "--roughness") src.roughness = value;
        else if (arg == "--ao") src.occlusion = value;
        else if (arg == "--height") src.height = value;
        else if (!parse_number(value, n) || n < 0) return usage_error("bad number for " + std::string(arg));
        else if (arg == "--size") o.size = static_cast<std::uint32_t>(n);
        else if (arg == "--threads") o.textures.threads = static_cast<std::uint32_t>(n);
        else return usage_error("unknown option: " + std::string(arg));
    }
    const auto start = std::chrono::steady_clock::now();
    auto stats = engine::cook::cook_terrain_layer(src, argv[2], o);
    if (!stats) {
        std::fprintf(stderr, "%s\n", stats.error().message.c_str());
        return 1;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("terrain layer -> %s in %.1fs\n  total   %llu -> %llu bytes\n", argv[2], secs,
                static_cast<unsigned long long>(stats->input_bytes),
                static_cast<unsigned long long>(stats->output_bytes));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--terrain-layer") {
        return cook_terrain_layer_cli(argc, argv);
    }
    if (argc < 3) return usage_error("missing input or output");
    engine::cook::CookOptions o;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        double n = 0;
        if (arg == "--no-mips") {
            o.textures.mipmaps = false;
        } else if (arg == "--no-textures") {
            o.textures.enabled = false;
        } else if (arg == "--no-optimize") {
            o.meshes.optimize = false;
        } else if (arg == "--no-quantize") {
            o.meshes.quantize = false;
        } else if (arg == "--no-compress") {
            o.meshes.compress = false;
        } else if (!has_value) {
            return usage_error("unknown option or missing value: " + std::string(arg));
        } else if (arg == "--color-codec" || arg == "--normal-codec") {
            auto& codec = arg == "--color-codec" ? o.textures.color_codec : o.textures.normal_codec;
            if (!parse_codec(argv[++i], codec)) return usage_error("bad codec");
        } else if (arg == "--normal-layout") {
            const std::string_view v = argv[++i];
            if (v != "xy" && v != "rgb") return usage_error("bad normal layout");
            o.textures.normal_xy_layout = v == "xy";
        } else if (!parse_number(argv[++i], n) || n < 0) {
            return usage_error("bad number for " + std::string(arg));
        } else if (arg == "--etc1s-quality") {
            o.textures.etc1s_quality = static_cast<std::uint32_t>(n);
        } else if (arg == "--uastc-level") {
            o.textures.uastc_level = static_cast<std::uint32_t>(n);
        } else if (arg == "--uastc-rdo") {
            o.textures.uastc_rdo = static_cast<float>(n);
        } else if (arg == "--zstd") {
            o.textures.zstd_level = static_cast<std::uint32_t>(n);
        } else if (arg == "--threads") {
            o.textures.threads = static_cast<std::uint32_t>(n);
        } else if (arg == "--position-bits") {
            o.meshes.position_bits = static_cast<int>(n);
        } else if (arg == "--normal-bits") {
            o.meshes.normal_bits = static_cast<int>(n);
        } else if (arg == "--simplify") {
            if (n <= 0 || n > 1) return usage_error("--simplify expects a ratio in (0, 1]");
            o.meshes.simplify_ratio = static_cast<float>(n);
        } else if (arg == "--simplify-error") {
            if (n > 1) return usage_error("--simplify-error expects a value in [0, 1]");
            o.meshes.simplify_error = static_cast<float>(n);
        } else if (arg == "--blend-to-mask") {
            if (n > 1) return usage_error("--blend-to-mask expects a cutoff in [0, 1]");
            o.materials.blend_to_mask_cutoff = static_cast<float>(n);
        } else {
            return usage_error("unknown option: " + std::string(arg));
        }
    }

    const auto start = std::chrono::steady_clock::now();
    auto stats = engine::cook::cook_gltf(argv[1], argv[2], o);
    if (!stats) {
        std::fprintf(stderr, "%s\n", stats.error().message.c_str());
        return 1;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%s -> %s in %.1fs\n"
                "  total   %llu -> %llu bytes\n"
                "  images  %u encoded, %llu -> %llu bytes\n"
                "  meshes  %u primitives cooked (%u skipped), %llu -> %llu bytes, "
                "%llu -> %llu triangles\n",
                argv[1], argv[2], secs, static_cast<unsigned long long>(stats->input_bytes),
                static_cast<unsigned long long>(stats->output_bytes), stats->images_encoded,
                static_cast<unsigned long long>(stats->image_bytes_before),
                static_cast<unsigned long long>(stats->image_bytes_after), stats->primitives_cooked,
                stats->primitives_skipped, static_cast<unsigned long long>(stats->mesh_bytes_before),
                static_cast<unsigned long long>(stats->mesh_bytes_after),
                static_cast<unsigned long long>(stats->triangles_before),
                static_cast<unsigned long long>(stats->triangles_after));
    return 0;
}
