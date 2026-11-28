// quat-asset-gen: writes the procedural nature models (asset-gen/nature_models)
// as .glb files.
//
//   quat-asset-gen <out_dir> [--seed N]
//
// The committed assets/meshes/nature/ files are this tool's output with the
// default seed (1); engine_tests checks they are current.

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "asset-gen/glb_writer.hpp"
#include "asset-gen/nature_models.hpp"
#include "engine/core/utf8_path.hpp"

int main(int argc, char** argv) {
    using namespace quat::asset_gen;
    const char* out_dir = nullptr;
    std::uint32_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            const auto res = std::from_chars(v.data(), v.data() + v.size(), seed);
            if (res.ec != std::errc{} || res.ptr != v.data() + v.size()) {
                std::fprintf(stderr, "quat-asset-gen: bad --seed '%s'\n", argv[i]);
                return 2;
            }
        } else if (out_dir == nullptr && !arg.starts_with("--")) {
            out_dir = argv[i];
        } else {
            std::fprintf(stderr, "usage: quat-asset-gen <out_dir> [--seed N]\n");
            return 2;
        }
    }
    if (out_dir == nullptr) {
        std::fprintf(stderr, "usage: quat-asset-gen <out_dir> [--seed N]\n");
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "quat-asset-gen: cannot create '%s': %s\n", out_dir,
                     ec.message().c_str());
        return 1;
    }
    const auto catalogue = nature_catalogue();
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        const Model model = catalogue[i].build(asset_seed(seed, i));
        const std::vector<std::byte> bytes = write_glb(model);
        const std::filesystem::path path = std::filesystem::path(out_dir) / catalogue[i].file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::fprintf(stderr, "quat-asset-gen: failed to write '%s'\n",
                         engine::core::path_to_utf8(path).c_str());
            return 1;
        }
        std::printf("%-22s %6zu triangles %7zu bytes\n", std::string(catalogue[i].file).c_str(),
                    model.triangle_count(), bytes.size());
    }
    return 0;
}
