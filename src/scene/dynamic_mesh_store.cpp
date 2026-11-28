#include "engine/scene/dynamic_mesh.hpp"

#include <algorithm>
#include <format>

#include "scene/dynamic_mesh_store_impl.hpp"

namespace engine::scene {

namespace {

std::size_t gpu_bytes(const geometry::MeshData& m) {
    return m.vertices.size() * sizeof(asset::Vertex) + m.indices.size() * sizeof(std::uint32_t);
}

} // namespace

bool valid_dynamic_mesh_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

DynamicMeshStore::DynamicMeshStore() : DynamicMeshStore(Config{}) {}
DynamicMeshStore::DynamicMeshStore(const Config& config) : impl_(std::make_unique<Impl>(config)) {}
DynamicMeshStore::DynamicMeshStore(DynamicMeshStore&&) noexcept = default;
DynamicMeshStore& DynamicMeshStore::operator=(DynamicMeshStore&&) noexcept = default;
DynamicMeshStore::~DynamicMeshStore() = default;

const DynamicMeshStore::Config& DynamicMeshStore::config() const { return impl_->config; }

std::expected<std::uint64_t, core::Error> DynamicMeshStore::submit(std::string_view id, geometry::MeshData mesh) {
    if (!valid_dynamic_mesh_id(id)) {
        return std::unexpected(core::make_error(
            std::format("dynamic mesh id \"{}\" must be 1-128 printable ASCII characters without spaces", id)));
    }
    if (auto valid = geometry::validate(mesh); !valid) {
        return std::unexpected(core::make_error(std::format("dynamic mesh \"{}\": {}", id, valid.error().message)));
    }
    if (gpu_bytes(mesh) > impl_->config.max_mesh_bytes) {
        return std::unexpected(core::make_error(std::format("dynamic mesh \"{}\": {} bytes is over the {} byte limit",
                                                            id, gpu_bytes(mesh), impl_->config.max_mesh_bytes)));
    }
    Impl& m = *impl_;
    auto [it, inserted] = m.entries.try_emplace(std::string(id));
    Impl::Entry& e = it->second;
    e.data = std::move(mesh);
    e.version += 1;
    e.hash = geometry::mesh_hash(e.data);
    e.bounds = geometry::compute_bounds(e.data);
    e.error.clear();
    if (Impl::Upload* old = m.pending(id)) {
        m.retire(std::move(old->slot)); // superseded; its copies may be in flight
        m.retire(std::move(old->base));
        *old = Impl::Upload{};
        old->id = std::string(id);
        old->version = e.version;
    } else {
        Impl::Upload upload;
        upload.id = std::string(id);
        upload.version = e.version;
        m.uploads.push_back(std::move(upload));
    }
    return e.version;
}

std::expected<std::uint64_t, core::Error>
DynamicMeshStore::update_vertices(std::string_view id, std::uint32_t first, std::span<const asset::Vertex> vertices) {
    Impl& m = *impl_;
    const auto it = m.entries.find(id);
    if (it == m.entries.end()) {
        return std::unexpected(core::make_error(std::format("dynamic mesh \"{}\" does not exist", id)));
    }
    Impl::Entry& e = it->second;
    if (static_cast<std::size_t>(first) + vertices.size() > e.data.vertices.size()) {
        return std::unexpected(core::make_error(std::format(
            "dynamic mesh \"{}\": vertices [{}, {}) are outside its {} vertices", id, first,
            static_cast<std::size_t>(first) + vertices.size(), e.data.vertices.size())));
    }
    geometry::MeshData updated = e.data;
    std::copy(vertices.begin(), vertices.end(), updated.vertices.begin() + first);
    if (auto valid = geometry::validate(updated); !valid) {
        return std::unexpected(core::make_error(std::format("dynamic mesh \"{}\": {}", id, valid.error().message)));
    }
    // A region update needs the previous version resident and no other
    // upload of this mesh in flight; otherwise it is a full upload.
    const bool region = m.pending(id) == nullptr && e.resident && e.resident_version == e.version;
    MeshSlot base = region ? e.resident : nullptr;
    auto version = submit(id, std::move(updated));
    if (!version) {
        return version;
    }
    if (region && !vertices.empty()) {
        Impl::Upload* u = m.pending(id);
        u->region = true;
        u->base = std::move(base);
        u->dirty_begin = static_cast<std::size_t>(first) * sizeof(asset::Vertex);
        u->dirty_end = u->dirty_begin + vertices.size() * sizeof(asset::Vertex);
    }
    return version;
}

bool DynamicMeshStore::remove(std::string_view id) {
    Impl& m = *impl_;
    const auto it = m.entries.find(id);
    if (it == m.entries.end()) {
        return false;
    }
    m.retire(std::move(it->second.resident));
    for (auto u = m.uploads.begin(); u != m.uploads.end(); ++u) {
        if (u->id == id) {
            m.retire(std::move(u->slot));
            m.retire(std::move(u->base));
            m.uploads.erase(u);
            break;
        }
    }
    m.entries.erase(it);
    return true;
}

void DynamicMeshStore::clear() {
    impl_->uploads.clear();
    impl_->entries.clear();
    impl_->retired.flush(); // the caller guarantees an idle device
}

bool DynamicMeshStore::contains(std::string_view id) const { return impl_->entries.contains(id); }

const geometry::MeshData* DynamicMeshStore::mesh(std::string_view id) const {
    const auto it = impl_->entries.find(id);
    return it == impl_->entries.end() ? nullptr : &it->second.data;
}

std::optional<DynamicMeshInfo> DynamicMeshStore::info(std::string_view id) const {
    const auto it = impl_->entries.find(id);
    if (it == impl_->entries.end()) {
        return std::nullopt;
    }
    const Impl::Entry& e = it->second;
    return DynamicMeshInfo{it->first,           e.version, e.resident_version, e.data.vertices.size(),
                           e.data.triangle_count(), gpu_bytes(e.data), e.bounds, e.hash};
}

std::vector<DynamicMeshInfo> DynamicMeshStore::list() const {
    std::vector<DynamicMeshInfo> out;
    for (const auto& [id, e] : impl_->entries) {
        out.push_back(*info(id));
    }
    return out;
}

DynamicMeshStats DynamicMeshStore::stats() const {
    DynamicMeshStats s;
    s.meshes = impl_->entries.size();
    s.pending_uploads = impl_->uploads.size();
    for (const Impl::Upload& u : impl_->uploads) {
        const auto it = impl_->entries.find(u.id);
        if (it != impl_->entries.end()) {
            s.pending_bytes += gpu_bytes(it->second.data);
        }
    }
    for (const auto& [id, e] : impl_->entries) {
        if (e.resident) {
            s.resident_bytes += gpu_bytes(e.data);
        }
    }
    s.uploaded_bytes_last_frame = impl_->uploaded_last_frame;
    return s;
}

} // namespace engine::scene
