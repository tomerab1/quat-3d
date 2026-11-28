#pragma once

// Terrain layer texture sets (ROADMAP R26.2): the cooked KTX2 textures of each
// set the terrain material uses, as three texture arrays (sRGB albedo, X/Y
// normal, packed roughness / occlusion / height) with one slice per set.
//
// Sets live in <data root>/assets/terrain/sets/<set>/{albedo,normal,packed}.ktx2,
// written by `quat-cook --terrain-layer` (scripts/fetch-polyhaven.py cooks
// them from the CC0 Poly Haven sources assets/terrain/layers.json lists).
// They decode and transcode on worker threads (the device's best format: BC7
// / BC5, ASTC 4x4, else RGBA8); the arrays are rebuilt on the main thread
// once every requested set has finished. A set that is missing or does not
// match the others' extent / format / mip count is left out: its layers fall
// back to their tinted procedural colour (slice -1). Until the first build
// the arrays are 1x1 placeholders.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/asset/texture_asset.hpp"
#include "engine/asset/texture_data.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::renderer {

class TerrainTextures {
public:
    [[nodiscard]] static std::expected<TerrainTextures, core::Error>
    create(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

    TerrainTextures() = default;
    TerrainTextures(TerrainTextures&&) noexcept = default;
    TerrainTextures& operator=(TerrainTextures&&) noexcept = default;
    TerrainTextures(const TerrainTextures&) = delete;
    TerrainTextures& operator=(const TerrainTextures&) = delete;

    // Directory holding assets/terrain/sets (the engine's data root). Sets
    // already requested are looked up again.
    void set_root(const std::filesystem::path& root);

    // The sets the material uses (any order, duplicates allowed). Starts
    // loading sets not seen before; sets no longer used stay cached.
    void request(std::span<const std::string> sets);

    // Main-thread upkeep: collects finished loads and, once none is pending,
    // rebuilds the arrays when the set of loaded textures changed. Returns
    // true when the views / slices changed (descriptors must be rewritten).
    // Old arrays are kept alive for a few frames.
    [[nodiscard]] bool poll();

    // Blocks until every requested set has loaded or failed, then poll()s
    // (tools and tests that need the final arrays at once).
    [[nodiscard]] bool wait();

    // Array slice of `set`, or -1 when it has no textures.
    [[nodiscard]] int slice_of(const std::string& set) const;
    [[nodiscard]] float mean_mip() const { return static_cast<float>(mip_levels_ - 1); }
    [[nodiscard]] std::uint32_t textured_sets() const { return layers_; }

    [[nodiscard]] VkImageView albedo_view() const { return albedo_.view.handle(); }
    [[nodiscard]] VkImageView normal_view() const { return normal_.view.handle(); }
    [[nodiscard]] VkImageView packed_view() const { return packed_.view.handle(); }
    [[nodiscard]] VkSampler sampler() const { return sampler_.handle(); }

private:
    struct SetTextures {
        asset::TextureData albedo;
        asset::TextureData normal;
        asset::TextureData packed;
    };
    struct Entry {
        std::future<std::expected<SetTextures, core::Error>> pending;
        std::optional<SetTextures> loaded;
        bool failed = false;
    };
    struct Arrays {
        asset::TextureArray albedo;
        asset::TextureArray normal;
        asset::TextureArray packed;
    };

    [[nodiscard]] std::expected<void, core::Error> rebuild();
    void start(const std::string& set, Entry& entry);

    rhi::GpuAllocator*         allocator_ = nullptr;
    rhi::TransferContext       transfer_{}; // handles only: copied
    std::filesystem::path      root_;
    std::map<std::string, Entry, std::less<>> sets_;
    std::map<std::string, int, std::less<>> slices_;
    bool                       dirty_ = false;

    asset::TextureArray albedo_;
    asset::TextureArray normal_;
    asset::TextureArray packed_;
    std::uint32_t       layers_ = 0;
    std::uint32_t       mip_levels_ = 1;
    rhi::Sampler        sampler_;
    std::vector<std::pair<int, Arrays>> retired_;
};

} // namespace engine::renderer
