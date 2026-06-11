#pragma once

// Scene serialization (editor save/load). JSON v1: a flat entity array with
// parent indices and the engine components; mesh references go through the
// MeshSource provenance component (AssetManager cache keys + optional glTF
// path). Loading clears the scene, re-resolves glTF assets by instantiating
// their files into a scratch scene (cache-key population), and rebuilds the
// hierarchy. Skinned meshes/animators are not yet serialized.

#include <expected>
#include <filesystem>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {
class AssetManager;
}

namespace engine::scene {

class Scene;

// Writes the scene (all entities) as JSON. Creates parent directories.
[[nodiscard]] std::expected<void, core::Error> save_scene(const Scene& scene,
                                                          const std::filesystem::path& path);

// Clears `scene` and loads `path` into it. glTF-sourced assets are re-resolved
// through `assets` (loading their files if not already cached); code-created
// asset keys must already be in the cache or the entity loads mesh-less.
// Ticks the scene once so world transforms are valid on return.
[[nodiscard]] std::expected<void, core::Error>
load_scene(Scene& scene, const std::filesystem::path& path, rhi::GpuAllocator& allocator,
           const rhi::TransferContext& transfer, asset::AssetManager& assets);

// Round trip: build a scene (hierarchy, mesh source, collider, lights), save,
// clear, load, verify entities/components/mesh handles came back.
[[nodiscard]] std::expected<void, core::Error>
run_scene_io_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

} // namespace engine::scene
