// quat-cook: offline glTF cooker (KTX2/Basis textures, meshoptimizer meshes).
// Usage is documented in docs/DEVELOPMENT.md ("Cooking assets").

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "engine/cook/gltf_cooker.hpp"

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
    "  --threads N                  encoder threads (default: all cores)\n";

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

} // namespace

int main(int argc, char** argv) {
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
                "  meshes  %u primitives cooked (%u skipped), %llu -> %llu bytes\n",
                argv[1], argv[2], secs, static_cast<unsigned long long>(stats->input_bytes),
                static_cast<unsigned long long>(stats->output_bytes), stats->images_encoded,
                static_cast<unsigned long long>(stats->image_bytes_before),
                static_cast<unsigned long long>(stats->image_bytes_after), stats->primitives_cooked,
                stats->primitives_skipped, static_cast<unsigned long long>(stats->mesh_bytes_before),
                static_cast<unsigned long long>(stats->mesh_bytes_after));
    return 0;
}
