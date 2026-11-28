// Terrain layer texture sets as texture arrays (see
// engine/renderer/terrain_textures.hpp).

#include "engine/renderer/terrain_textures.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <system_error>

#include "engine/asset/ktx2_texture.hpp"
#include "engine/core/utf8_path.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"terrain textures: " + std::move(what)});
}

// Frames an array replaced by a rebuild stays alive (frames in flight + 1).
constexpr int k_retire_frames = 3;

// A 1x1 RGBA8 placeholder array (never sampled: every slice is -1).
[[nodiscard]] std::expected<asset::TextureArray, core::Error>
placeholder(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
            asset::TextureColorSpace space, std::uint32_t texel) {
    asset::TextureData d;
    d.pixels = asset::PixelBuffer::allocate(4);
    if (d.pixels.empty()) return fail("out of memory");
    std::memcpy(d.pixels.data(), &texel, 4);
    d.width = 1;
    d.height = 1;
    d.color_space = space;
    d.format = asset::TexelFormat::rgba8;
    d.levels.push_back({0, 4, 1, 1});
    const asset::TextureData* layers[] = {&d};
    return asset::upload_texture_array(layers, allocator, transfer);
}

[[nodiscard]] bool same_layout(const asset::TextureData& a, const asset::TextureData& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.color_space == b.color_space && a.swizzle == b.swizzle &&
           a.levels.size() == b.levels.size();
}

} // namespace

std::expected<TerrainTextures, core::Error>
TerrainTextures::create(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    TerrainTextures out;
    out.allocator_ = &allocator;
    out.transfer_ = transfer;
    auto albedo = placeholder(allocator, transfer, asset::TextureColorSpace::srgb, 0xFF808080U);
    auto normal = placeholder(allocator, transfer, asset::TextureColorSpace::linear, 0xFFFF8080U);
    auto packed = placeholder(allocator, transfer, asset::TextureColorSpace::linear, 0xFF80FFE6U);
    if (!albedo) return std::unexpected(albedo.error());
    if (!normal) return std::unexpected(normal.error());
    if (!packed) return std::unexpected(packed.error());
    out.albedo_ = std::move(*albedo);
    out.normal_ = std::move(*normal);
    out.packed_ = std::move(*packed);
    // Repeat addressing over every mip, anisotropic: ground is seen at
    // grazing angles.
    auto sampler = rhi::create_sampler(transfer.device, rhi::SamplerAddress::repeat, 16,
                                       std::min(transfer.max_anisotropy, 16.0F));
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);
    return out;
}

void TerrainTextures::set_root(const std::filesystem::path& root) {
    if (root == root_) return;
    root_ = root;
    for (auto& [name, entry] : sets_) {
        if (entry.pending.valid()) continue; // finishes against the old root
        entry.loaded.reset();
        entry.failed = false;
        start(name, entry);
    }
    dirty_ = true;
}

void TerrainTextures::start(const std::string& set, Entry& entry) {
    const std::filesystem::path dir = root_ / "assets" / "terrain" / "sets" / set;
    asset::Ktx2DecodeOptions options;
    options.caps = transfer_.texture_caps;
    entry.pending = std::async(std::launch::async, [dir, options]() mutable
                               -> std::expected<SetTextures, core::Error> {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            return fail("set not cooked (" + core::path_to_utf8(dir) + ")");
        }
        SetTextures t;
        const auto load = [&](const char* file, asset::TextureRole role,
                              asset::TextureColorSpace space, asset::TextureData& out)
            -> std::expected<void, core::Error> {
            asset::Ktx2DecodeOptions o = options;
            o.role = role;
            o.color_space = space;
            auto data = asset::load_ktx2_file(dir / file, o);
            if (!data) return fail(std::string(file) + ": " + data.error().message);
            if (data->levels.empty()) return fail(std::string(file) + ": no mip levels");
            out = std::move(*data);
            return {};
        };
        if (auto r = load("albedo.ktx2", asset::TextureRole::color, asset::TextureColorSpace::srgb,
                          t.albedo);
            !r) {
            return std::unexpected(r.error());
        }
        if (auto r = load("normal.ktx2", asset::TextureRole::normal,
                          asset::TextureColorSpace::linear, t.normal);
            !r) {
            return std::unexpected(r.error());
        }
        if (auto r = load("packed.ktx2", asset::TextureRole::color,
                          asset::TextureColorSpace::linear, t.packed);
            !r) {
            return std::unexpected(r.error());
        }
        return t;
    });
}

void TerrainTextures::request(std::span<const std::string> sets) {
    for (const std::string& set : sets) {
        if (set.empty() || sets_.contains(set)) continue;
        Entry& entry = sets_[set];
        start(set, entry);
    }
}

bool TerrainTextures::poll() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (--it->first <= 0) {
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
    bool pending = false;
    for (auto& [name, entry] : sets_) {
        if (!entry.pending.valid()) continue;
        if (entry.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            pending = true;
            continue;
        }
        auto result = entry.pending.get();
        if (result) {
            entry.loaded = std::move(*result);
            std::fprintf(stderr, "[terrain] texture set \"%s\" loaded\n", name.c_str());
        } else {
            entry.failed = true;
            std::fprintf(stderr, "[terrain] texture set \"%s\" unavailable (%s): untextured\n",
                         name.c_str(), result.error().message.c_str());
        }
        dirty_ = true;
    }
    if (pending || !dirty_) return false;
    dirty_ = false;
    if (auto r = rebuild(); !r) {
        std::fprintf(stderr, "[terrain] %s\n", r.error().message.c_str());
        return false;
    }
    return true;
}

bool TerrainTextures::wait() {
    for (auto& [name, entry] : sets_) {
        if (entry.pending.valid()) entry.pending.wait();
    }
    return poll();
}

std::expected<void, core::Error> TerrainTextures::rebuild() {
    std::vector<const asset::TextureData*> albedo;
    std::vector<const asset::TextureData*> normal;
    std::vector<const asset::TextureData*> packed;
    std::map<std::string, int, std::less<>> slices;
    for (const auto& [name, entry] : sets_) {
        if (!entry.loaded) continue;
        const SetTextures& t = *entry.loaded;
        if (!albedo.empty() && (!same_layout(*albedo[0], t.albedo) ||
                                !same_layout(*normal[0], t.normal) ||
                                !same_layout(*packed[0], t.packed))) {
            std::fprintf(stderr,
                         "[terrain] texture set \"%s\" differs in size / format / mips from "
                         "the others; left untextured (cook every set at the same size)\n",
                         name.c_str());
            continue;
        }
        slices[name] = static_cast<int>(albedo.size());
        albedo.push_back(&t.albedo);
        normal.push_back(&t.normal);
        packed.push_back(&t.packed);
    }
    if (albedo.empty()) {
        slices_.clear();
        return {};
    }
    auto a = asset::upload_texture_array(albedo, *allocator_, transfer_);
    if (!a) return fail("albedo array: " + a.error().message);
    auto n = asset::upload_texture_array(normal, *allocator_, transfer_);
    if (!n) return fail("normal array: " + n.error().message);
    auto p = asset::upload_texture_array(packed, *allocator_, transfer_);
    if (!p) return fail("packed array: " + p.error().message);
    Arrays old{std::move(albedo_), std::move(normal_), std::move(packed_)};
    retired_.emplace_back(k_retire_frames, std::move(old));
    albedo_ = std::move(*a);
    normal_ = std::move(*n);
    packed_ = std::move(*p);
    layers_ = albedo_.layers;
    mip_levels_ = albedo_.mip_levels;
    slices_ = std::move(slices);
    return {};
}

int TerrainTextures::slice_of(const std::string& set) const {
    const auto it = slices_.find(set);
    return it != slices_.end() ? it->second : -1;
}

} // namespace engine::renderer
