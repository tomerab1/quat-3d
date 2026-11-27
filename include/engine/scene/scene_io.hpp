#pragma once

// Scene serialization (editor save/load). JSON v1: a flat entity array with
// parent indices and the engine components; mesh references go through the
// MeshSource provenance component (AssetManager cache keys + optional glTF
// path). Loading clears the scene, re-resolves glTF assets by instantiating
// their files into a scratch scene (cache-key population), and rebuilds the
// hierarchy. Skinned meshes/animators are not yet serialized. The renderer's
// settings (A8) travel in an optional, versioned "render_settings" block
// (render_settings_json.hpp); files without it load with the defaults.

#include <expected>
#include <filesystem>
#include <string>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {
class AssetManager;
}
namespace engine::renderer {
struct RenderSettings;
}

namespace engine::scene {

class Scene;

// Scene-level data that is not an entity (file key "environment"): the
// HDRI panorama of the lighting environment (ROADMAP R21), a path relative
// to the project root ("" = none). The renderer's settings choose whether
// it is used (render_settings.environment.source).
struct SceneEnvironment {
    std::string hdri;

    static constexpr std::size_t k_max_path = 4096;
};

// Writes the scene (all entities) as JSON, plus `render_settings` and
// `environment` when given. Creates parent directories.
[[nodiscard]] std::expected<void, core::Error>
save_scene(const Scene& scene, const std::filesystem::path& path,
           const renderer::RenderSettings* render_settings = nullptr,
           const SceneEnvironment* environment = nullptr);

// Clears `scene` and loads `path` into it. glTF-sourced assets are re-resolved
// through `assets` (loading their files if not already cached); code-created
// asset keys must already be in the cache or the entity loads mesh-less.
// Ticks the scene once so world transforms are valid on return. With
// `render_settings`, they are replaced by the file's block (decoded leniently:
// unknown fields ignored, out-of-range numbers clamped), or reset to the
// defaults when the file has none; a malformed block, or one whose "version"
// is newer than k_render_settings_version, fails the load. With
// `environment`, it is replaced by the file's (empty when the file has
// none; a malformed block fails the load). Nothing changes when the load
// fails.
[[nodiscard]] std::expected<void, core::Error>
load_scene(Scene& scene, const std::filesystem::path& path, rhi::GpuAllocator& allocator,
           const rhi::TransferContext& transfer, asset::AssetManager& assets,
           renderer::RenderSettings* render_settings = nullptr,
           SceneEnvironment* environment = nullptr);

// Round trip: build a scene (hierarchy, mesh source, collider, lights), save,
// clear, load, verify entities/components/mesh handles came back.
[[nodiscard]] std::expected<void, core::Error>
run_scene_io_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

} // namespace engine::scene
