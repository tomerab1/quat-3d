// Terrain layer texture-set cook (see engine/cook/terrain_layer.hpp).

#include "engine/cook/terrain_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "engine/asset/image_decode.hpp"
#include "engine/core/utf8_path.hpp"

namespace engine::cook {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"terrain layer: " + std::move(what)});
}

// One channel-interleaved float image (4 channels, 0..1).
struct Plane {
    std::uint32_t      width = 0;
    std::uint32_t      height = 0;
    std::vector<float> px;

    [[nodiscard]] const float* at(std::uint32_t x, std::uint32_t y) const {
        return px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

[[nodiscard]] std::expected<std::vector<std::byte>, core::Error>
read_file(const std::filesystem::path& path, std::uint64_t& total) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot open " + core::path_to_utf8(path));
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    total += bytes.size();
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

// Decodes a PNG / JPEG into 8-bit values scaled to 0..1 (no colour
// conversion: sRGB stays sRGB-encoded until the encoder linearises it).
[[nodiscard]] std::expected<Plane, core::Error> load(const std::filesystem::path& path,
                                                     std::uint64_t& total) {
    auto bytes = read_file(path, total);
    if (!bytes) return std::unexpected(bytes.error());
    auto data = asset::decode_png_jpeg(*bytes, asset::TextureColorSpace::linear);
    if (!data) return fail(core::path_to_utf8(path) + ": " + data.error().message);
    Plane p{data->width, data->height, std::vector<float>(data->pixels.size())};
    for (std::size_t i = 0; i < p.px.size(); ++i) {
        p.px[i] = static_cast<float>(std::to_integer<std::uint8_t>(data->pixels[i])) / 255.0F;
    }
    return p;
}

// 2x2 box halving (power-of-two friendly prefilter).
[[nodiscard]] Plane halve(const Plane& s) {
    Plane d{std::max(s.width / 2, 1U), std::max(s.height / 2, 1U), {}};
    d.px.resize(static_cast<std::size_t>(d.width) * d.height * 4);
    for (std::uint32_t y = 0; y < d.height; ++y) {
        for (std::uint32_t x = 0; x < d.width; ++x) {
            const std::uint32_t x0 = std::min(2 * x, s.width - 1), x1 = std::min(2 * x + 1, s.width - 1);
            const std::uint32_t y0 = std::min(2 * y, s.height - 1), y1 = std::min(2 * y + 1, s.height - 1);
            for (int c = 0; c < 4; ++c) {
                d.px[(static_cast<std::size_t>(y) * d.width + x) * 4 + c] =
                    0.25F * (s.at(x0, y0)[c] + s.at(x1, y0)[c] + s.at(x0, y1)[c] + s.at(x1, y1)[c]);
            }
        }
    }
    return d;
}

// Bilinear resample with wrap-around (the sets tile).
[[nodiscard]] Plane resample(const Plane& s, std::uint32_t size) {
    Plane d{size, size, std::vector<float>(static_cast<std::size_t>(size) * size * 4)};
    const float sx = static_cast<float>(s.width) / static_cast<float>(size);
    const float sy = static_cast<float>(s.height) / static_cast<float>(size);
    for (std::uint32_t y = 0; y < size; ++y) {
        const float fy = (static_cast<float>(y) + 0.5F) * sy - 0.5F;
        const float fly = std::floor(fy);
        const float ty = fy - fly;
        const auto y0 = static_cast<std::uint32_t>((static_cast<std::int64_t>(fly) + s.height) % s.height);
        const std::uint32_t y1 = (y0 + 1) % s.height;
        for (std::uint32_t x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5F) * sx - 0.5F;
            const float flx = std::floor(fx);
            const float tx = fx - flx;
            const auto x0 = static_cast<std::uint32_t>((static_cast<std::int64_t>(flx) + s.width) % s.width);
            const std::uint32_t x1 = (x0 + 1) % s.width;
            for (int c = 0; c < 4; ++c) {
                const float top = s.at(x0, y0)[c] * (1 - tx) + s.at(x1, y0)[c] * tx;
                const float bot = s.at(x0, y1)[c] * (1 - tx) + s.at(x1, y1)[c] * tx;
                d.px[(static_cast<std::size_t>(y) * size + x) * 4 + c] = top * (1 - ty) + bot * ty;
            }
        }
    }
    return d;
}

[[nodiscard]] Plane fit(Plane p, std::uint32_t size) {
    while (p.width >= 2 * size && p.height >= 2 * size) p = halve(p);
    if (p.width == size && p.height == size) return p;
    return resample(p, size);
}

[[nodiscard]] std::vector<std::byte> to_rgba8(const Plane& p) {
    std::vector<std::byte> out(p.px.size());
    for (std::size_t i = 0; i < p.px.size(); ++i) {
        out[i] = static_cast<std::byte>(std::lround(std::clamp(p.px[i], 0.0F, 1.0F) * 255.0F));
    }
    return out;
}

[[nodiscard]] std::expected<void, core::Error> write(const std::filesystem::path& path,
                                                     const std::vector<std::byte>& bytes,
                                                     std::uint64_t& total) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write " + core::path_to_utf8(path));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return fail("cannot write " + core::path_to_utf8(path));
    total += bytes.size();
    return {};
}

[[nodiscard]] std::expected<std::optional<Plane>, core::Error>
load_optional(const std::filesystem::path& path, std::uint32_t size, std::uint64_t& total) {
    if (path.empty()) return std::optional<Plane>{};
    auto p = load(path, total);
    if (!p) return std::unexpected(p.error());
    return std::optional<Plane>{fit(std::move(*p), size)};
}

} // namespace

std::expected<TerrainLayerCookStats, core::Error>
cook_terrain_layer(const TerrainLayerSources& src, const std::filesystem::path& out_dir,
                   const TerrainLayerCookOptions& options) {
    const std::uint32_t size = options.size;
    if (size < 4 || size > 8192 || (size & (size - 1)) != 0) {
        return fail("size must be a power of two in 4..8192");
    }
    if (src.albedo.empty() || src.normal.empty()) return fail("albedo and normal maps are required");
    TerrainLayerCookStats stats;
    auto albedo = load_optional(src.albedo, size, stats.input_bytes);
    if (!albedo) return std::unexpected(albedo.error());
    auto normal = load_optional(src.normal, size, stats.input_bytes);
    if (!normal) return std::unexpected(normal.error());
    auto arm = load_optional(src.arm, size, stats.input_bytes);
    if (!arm) return std::unexpected(arm.error());
    auto rough = load_optional(src.roughness, size, stats.input_bytes);
    if (!rough) return std::unexpected(rough.error());
    auto ao = load_optional(src.occlusion, size, stats.input_bytes);
    if (!ao) return std::unexpected(ao.error());
    auto height = load_optional(src.height, size, stats.input_bytes);
    if (!height) return std::unexpected(height.error());

    // Normals: renormalise the resampled vectors; +Y along +v.
    Plane& n = **normal;
    for (std::size_t i = 0; i < n.px.size(); i += 4) {
        float v[3] = {n.px[i] * 2 - 1, n.px[i + 1] * 2 - 1, n.px[i + 2] * 2 - 1};
        if (src.normal_opengl) v[1] = -v[1];
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-6F) {
            v[0] = 0;
            v[1] = 0;
            v[2] = 1;
        } else {
            for (float& c : v) c /= len;
        }
        for (int c = 0; c < 3; ++c) n.px[i + c] = v[c] * 0.5F + 0.5F;
        n.px[i + 3] = 1.0F;
    }

    // Packed: R roughness, G occlusion, B height (normalised), A 1.
    float hmin = 1.0F;
    float hmax = 0.0F;
    if (*height) {
        for (std::size_t i = 0; i < (*height)->px.size(); i += 4) {
            hmin = std::min(hmin, (*height)->px[i]);
            hmax = std::max(hmax, (*height)->px[i]);
        }
    }
    Plane packed{size, size, std::vector<float>(static_cast<std::size_t>(size) * size * 4)};
    for (std::size_t i = 0; i < packed.px.size(); i += 4) {
        float r = 1.0F;
        float o = 1.0F;
        if (*arm) {
            o = (*arm)->px[i];
            r = (*arm)->px[i + 1];
        }
        if (*rough) r = (*rough)->px[i];
        if (*ao) o = (*ao)->px[i];
        float h = 0.5F;
        if (*height) h = hmax > hmin ? ((*height)->px[i] - hmin) / (hmax - hmin) : 0.5F;
        packed.px[i] = r;
        packed.px[i + 1] = o;
        packed.px[i + 2] = h;
        packed.px[i + 3] = 1.0F;
    }
    // Albedo is opaque.
    for (std::size_t i = 3; i < (*albedo)->px.size(); i += 4) (*albedo)->px[i] = 1.0F;

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) return fail("cannot create " + core::path_to_utf8(out_dir) + ": " + ec.message());
    struct Out {
        const char*              file;
        const Plane*             plane;
        asset::TextureRole       role;
        asset::TextureColorSpace space;
    };
    const Out outs[] = {
        {"albedo.ktx2", &**albedo, asset::TextureRole::color, asset::TextureColorSpace::srgb},
        {"normal.ktx2", &n, asset::TextureRole::normal, asset::TextureColorSpace::linear},
        {"packed.ktx2", &packed, asset::TextureRole::color, asset::TextureColorSpace::linear},
    };
    TextureCookOptions tex = options.textures;
    tex.mipmaps = true;
    tex.normal_xy_layout = true;
    for (const Out& o : outs) {
        auto encoded = encode_ktx2(to_rgba8(*o.plane), size, size, o.role, o.space, tex);
        if (!encoded) return fail(std::string(o.file) + ": " + encoded.error().message);
        if (auto w = write(out_dir / o.file, *encoded, stats.output_bytes); !w) {
            return std::unexpected(w.error());
        }
    }
    return stats;
}

} // namespace engine::cook
