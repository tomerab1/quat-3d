#pragma once

// The frame loop's side of the DynamicMeshStore (ROADMAP R23.2). Not part of
// the SDK: games use engine/scene/dynamic_mesh.hpp; the application layer
// calls these once per frame, in this order:
//
//   dynamic_mesh_begin_frame(store, frame)      promote finished uploads,
//                                               free retired buffers
//   resolve_dynamic_meshes(...)                 DynamicMesh -> MeshRenderer
//   ... gather / draw ...
//   record_dynamic_mesh_uploads(store, ...)     stage this frame's budget of
//                                               uploads into the render graph

#include <cstdint>
#include <expected>
#include <string_view>

#include <entt/entity/registry.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/scene/dynamic_mesh.hpp"

namespace engine::rhi {
class GpuAllocator;
class RenderGraph;
struct TransferContext;
} // namespace engine::rhi

namespace engine::scene {

// Frees buffers no frame in flight reads any more and makes uploads whose
// copies were recorded in an earlier frame resident. `frame` increases by
// one per presented frame.
void dynamic_mesh_begin_frame(DynamicMeshStore& store, std::uint64_t frame);

// Stages up to the store's per-frame byte budget of pending uploads (in
// submission order; a mesh larger than the budget continues next frame) and
// adds the copies to `graph` as one transfer pass.
[[nodiscard]] std::expected<void, core::Error>
record_dynamic_mesh_uploads(DynamicMeshStore& store, rhi::GpuAllocator& allocator, rhi::RenderGraph& graph,
                            std::uint64_t frame);

// The resident GPU mesh of `id` (an invalid handle when none) and its version.
[[nodiscard]] asset::AssetHandle<asset::MeshAsset> dynamic_mesh_resident(const DynamicMeshStore& store,
                                                                         std::string_view id,
                                                                         std::uint64_t* version = nullptr);

// True when some DynamicMesh needs its generator run, its MeshRenderer
// (re)built or removed. Cheap: no GPU access.
[[nodiscard]] bool dynamic_meshes_need_resolve(const entt::registry& registry, const DynamicMeshStore& store);

// Runs pending generator specs (submitting their output), then points every
// DynamicMesh entity's MeshRenderer at its mesh's resident version with the
// component's material (materials shared through `assets` like Primitive
// materials). An entity whose mesh is not resident yet keeps what it drew.
// Returns the number of entities updated; generator errors are collected
// into one error after every entity was processed.
[[nodiscard]] std::expected<std::size_t, core::Error>
resolve_dynamic_meshes(entt::registry& registry, DynamicMeshStore& store, rhi::GpuAllocator& allocator,
                       const rhi::TransferContext& transfer, asset::AssetManager& assets);

} // namespace engine::scene
