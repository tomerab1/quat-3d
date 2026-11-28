#include <algorithm>
#include <cstring>
#include <format>
#include <span>

#include "engine/rhi/render_graph.hpp"
#include "engine/scene/dynamic_mesh_gpu.hpp"
#include "scene/dynamic_mesh_store_impl.hpp"

// Background uploads of DynamicMeshStore meshes (ROADMAP R23.2) through an
// rhi::StagingRing: each frame copies at most the store's byte budget, in
// submission order; a mesh is promoted (drawn) from the frame after its last
// copy was recorded, and the version it replaces is freed once the frames in
// flight that may still draw it have completed.

namespace engine::scene {

namespace {

using Impl = DynamicMeshStore::Impl;

std::span<const std::byte> vertex_bytes(const geometry::MeshData& m) {
    return std::as_bytes(std::span(m.vertices));
}

std::span<const std::byte> index_bytes(const geometry::MeshData& m) {
    return std::as_bytes(std::span(m.indices));
}

// Creates the destination buffers of `u` (sized for the latest data).
std::expected<void, core::Error> create_destination(Impl::Upload& u, const Impl::Entry& e,
                                                    rhi::GpuAllocator& allocator) {
    const VkBufferUsageFlags copy = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    auto vb = allocator.create_buffer(vertex_bytes(e.data).size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | copy,
                                      VMA_MEMORY_USAGE_AUTO);
    if (!vb) {
        return std::unexpected(vb.error());
    }
    auto ib = allocator.create_buffer(index_bytes(e.data).size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | copy,
                                      VMA_MEMORY_USAGE_AUTO);
    if (!ib) {
        return std::unexpected(ib.error());
    }
    u.slot = std::make_shared<asset::AssetSlot<asset::MeshAsset>>();
    u.slot->id = asset::asset_id_from_path(std::format("dynamic/{}#{}", u.id, u.version));
    asset::MeshAsset& mesh = u.slot->value;
    mesh.vertex_buffer = std::move(*vb);
    mesh.index_buffer = std::move(*ib);
    mesh.vertex_count = static_cast<std::uint32_t>(e.data.vertices.size());
    mesh.index_count = static_cast<std::uint32_t>(e.data.indices.size());
    mesh.bounds = e.bounds;
    mesh.submeshes = geometry::submeshes_or_default(e.data);
    return {};
}

// The byte stream an upload stages: vertices (only the dirty range for a
// region update), then indices (none for a region update: the topology is
// copied from the base).
struct Stream {
    std::span<const std::byte> vertices;
    std::size_t vertex_offset = 0; // where `vertices` goes in the vertex buffer
    std::span<const std::byte> indices;
    [[nodiscard]] std::size_t size() const { return vertices.size() + indices.size(); }
};

Stream stream_of(const Impl::Upload& u, const Impl::Entry& e) {
    if (u.region) {
        return Stream{vertex_bytes(e.data).subspan(u.dirty_begin, u.dirty_end - u.dirty_begin), u.dirty_begin, {}};
    }
    return Stream{vertex_bytes(e.data), 0, index_bytes(e.data)};
}

// Stages as much of `u` as the ring has room for. False when the budget ran
// out before the upload finished.
bool stage(Impl::Upload& u, const Impl::Entry& e, rhi::StagingRing& ring, std::size_t& uploaded) {
    const asset::MeshAsset& dst = u.slot->value;
    if (u.region && !u.base_copied) {
        // The unchanged parts come from the resident version, on the GPU.
        // Ranges are disjoint from the staged one, so the copies need no
        // ordering among themselves.
        const asset::MeshAsset& base = u.base->value;
        ring.copy(base.vertex_buffer, 0, dst.vertex_buffer, 0, u.dirty_begin);
        ring.copy(base.vertex_buffer, u.dirty_end, dst.vertex_buffer, u.dirty_end,
                  dst.vertex_buffer.size() - u.dirty_end);
        ring.copy(base.index_buffer, 0, dst.index_buffer, 0, dst.index_buffer.size());
        u.base_copied = true;
    }
    const Stream s = stream_of(u, e);
    while (u.staged < s.size()) {
        const std::size_t room = static_cast<std::size_t>(ring.remaining());
        if (room == 0) {
            return false;
        }
        const bool in_vertices = u.staged < s.vertices.size();
        const std::span<const std::byte> part =
            in_vertices ? s.vertices.subspan(u.staged) : s.indices.subspan(u.staged - s.vertices.size());
        const std::size_t n = std::min(room, part.size());
        const rhi::GpuBuffer& buffer = in_vertices ? dst.vertex_buffer : dst.index_buffer;
        const std::size_t offset = in_vertices ? s.vertex_offset + u.staged : u.staged - s.vertices.size();
        if (!ring.upload(part.first(n), buffer, offset)) {
            return false;
        }
        u.staged += n;
        uploaded += n;
    }
    return true;
}

} // namespace

void dynamic_mesh_begin_frame(DynamicMeshStore& store, std::uint64_t frame) {
    Impl& m = store.impl();
    m.frame = frame;
    m.retired.collect(frame);
    for (auto it = m.uploads.begin(); it != m.uploads.end();) {
        if (!it->done || it->done_frame >= frame) {
            ++it;
            continue;
        }
        Impl::Entry& e = m.entries.at(it->id);
        m.retire(std::move(e.resident));
        m.retire(std::move(it->base));
        it->slot->state.store(asset::AssetState::loaded, std::memory_order_release);
        e.resident = std::move(it->slot);
        e.resident_version = it->version;
        it = m.uploads.erase(it);
    }
}

std::expected<void, core::Error> record_dynamic_mesh_uploads(DynamicMeshStore& store, rhi::GpuAllocator& allocator,
                                                             rhi::RenderGraph& graph, std::uint64_t frame) {
    Impl& m = store.impl();
    m.uploaded_last_frame = 0;
    if (m.uploads.empty()) {
        return {};
    }
    if (!m.ring.valid()) {
        auto ring = rhi::StagingRing::create(allocator, m.config.frame_byte_budget, m.config.frames_in_flight);
        if (!ring) {
            return std::unexpected(ring.error());
        }
        m.ring = std::move(*ring);
    }
    m.ring.begin_frame(frame);
    std::string failures;
    for (auto it = m.uploads.begin(); it != m.uploads.end();) {
        Impl::Upload& u = *it;
        if (u.done) {
            ++it;
            continue;
        }
        Impl::Entry& e = m.entries.at(u.id);
        if (e.data.indices.empty()) {
            // Nothing to draw: the mesh becomes "resident" as empty.
            m.retire(std::move(e.resident));
            e.resident_version = u.version;
            it = m.uploads.erase(it);
            continue;
        }
        if (!u.slot) {
            if (auto created = create_destination(u, e, allocator); !created) {
                e.error = created.error().message;
                failures += (failures.empty() ? "" : "; ") + std::format("\"{}\": {}", u.id, e.error);
                it = m.uploads.erase(it);
                continue;
            }
        }
        if (!stage(u, e, m.ring, m.uploaded_last_frame)) {
            break; // budget spent: the rest waits for the next frame
        }
        u.done = true;
        u.done_frame = frame;
        ++it;
    }
    if (auto added = m.ring.add_to_graph(graph); !added) {
        return std::unexpected(added.error());
    }
    if (!failures.empty()) {
        return std::unexpected(core::make_error("dynamic mesh upload failed: " + failures));
    }
    return {};
}

asset::AssetHandle<asset::MeshAsset> dynamic_mesh_resident(const DynamicMeshStore& store, std::string_view id,
                                                           std::uint64_t* version) {
    const Impl& m = store.impl();
    const auto it = m.entries.find(id);
    if (version != nullptr) {
        *version = it == m.entries.end() ? 0 : it->second.resident_version;
    }
    if (it == m.entries.end() || !it->second.resident) {
        return {};
    }
    return asset::AssetHandle<asset::MeshAsset>(it->second.resident);
}

} // namespace engine::scene
