#pragma once

// DynamicMeshStore internals (ROADMAP R23.2): the latest CPU version of every
// mesh, the resident GPU version the renderer draws, and the uploads in
// between. Private to engine_scene; the frame loop drives it through
// engine/scene/dynamic_mesh_gpu.hpp.

#include <deque>
#include <map>
#include <memory>
#include <string>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/deferred_release_queue.hpp"
#include "engine/rhi/staging_ring.hpp"
#include "engine/scene/dynamic_mesh.hpp"

namespace engine::scene {

using MeshSlot = std::shared_ptr<asset::AssetSlot<asset::MeshAsset>>;

struct DynamicMeshStore::Impl {
    struct Entry {
        geometry::MeshData data; // latest version
        std::uint64_t version = 0;
        std::uint64_t hash = 0;
        asset::Aabb bounds{};
        MeshSlot resident;      // drawn now (null: nothing uploaded yet, or empty)
        std::uint64_t resident_version = 0;
        std::string error;      // last upload failure
    };

    // A version on its way to the GPU. Only the latest version of an id is
    // ever uploading: a newer submit supersedes (drops) the older upload.
    struct Upload {
        std::string id;
        std::uint64_t version = 0;
        MeshSlot slot;          // destination buffers (created when first scheduled)
        MeshSlot base;          // region updates: the resident version copied from
        std::size_t dirty_begin = 0; // region updates: vertex bytes [begin, end) staged
        std::size_t dirty_end = 0;
        bool region = false;
        bool base_copied = false;
        std::size_t staged = 0; // bytes of the staged stream done
        bool done = false;
        std::uint64_t done_frame = 0;
    };

    Config config;
    std::map<std::string, Entry, std::less<>> entries;
    std::deque<Upload> uploads;
    core::DeferredReleaseQueue retired;
    rhi::StagingRing ring;
    std::size_t uploaded_last_frame = 0;
    std::uint64_t frame = 0; // the frame being prepared (set by collect)

    explicit Impl(const Config& c) : config(c), retired(c.frames_in_flight) {}

    // Retires a GPU mesh as of the current frame: freed once no frame in
    // flight can read it (or copy into it).
    void retire(MeshSlot slot) {
        if (slot) {
            retired.push(frame, [s = std::move(slot)]() mutable { s.reset(); });
        }
    }

    [[nodiscard]] Upload* pending(std::string_view id) {
        for (Upload& u : uploads) {
            if (u.id == id) return &u;
        }
        return nullptr;
    }
};

} // namespace engine::scene
