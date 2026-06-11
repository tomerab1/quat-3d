#include "engine/nav/navmesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>
#include <recastnavigation/DetourNavMeshQuery.h>
#include <recastnavigation/Recast.h>

namespace engine::nav {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

struct NavMeshDeleter {
    void operator()(dtNavMesh* p) const { dtFreeNavMesh(p); }
};
struct QueryDeleter {
    void operator()(dtNavMeshQuery* p) const { dtFreeNavMeshQuery(p); }
};

} // namespace

struct NavMesh::Impl {
    std::unique_ptr<dtNavMesh, NavMeshDeleter>      mesh;
    std::unique_ptr<dtNavMeshQuery, QueryDeleter>   query;
    std::vector<std::pair<glm::vec3, glm::vec3>>    edges;
};

NavMesh::NavMesh() = default;
NavMesh::NavMesh(NavMesh&&) noexcept = default;
NavMesh& NavMesh::operator=(NavMesh&&) noexcept = default;
NavMesh::~NavMesh() = default;

bool NavMesh::valid() const { return impl_ != nullptr && impl_->mesh != nullptr; }

const std::vector<std::pair<glm::vec3, glm::vec3>>& NavMesh::debug_edges() const {
    static const std::vector<std::pair<glm::vec3, glm::vec3>> empty;
    return impl_ != nullptr ? impl_->edges : empty;
}

std::expected<NavMesh, core::Error> NavMesh::build(std::span<const glm::vec3> vertices,
                                                   std::span<const std::uint32_t> indices,
                                                   const NavMeshParams& params) {
    if (vertices.empty() || indices.size() < 3) return fail("navmesh: no input geometry");

    // Recast consumes raw float triples + int triangle indices.
    const auto* verts = reinterpret_cast<const float*>(vertices.data());
    const auto nverts = static_cast<int>(vertices.size());
    std::vector<int> tris(indices.begin(), indices.end());
    const int ntris = static_cast<int>(tris.size() / 3);

    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& v : vertices) {
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
    }

    rcConfig cfg{};
    cfg.cs = params.cell_size;
    cfg.ch = params.cell_height;
    cfg.walkableSlopeAngle = params.agent_max_slope_deg;
    cfg.walkableHeight = static_cast<int>(std::ceil(params.agent_height / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(params.agent_max_climb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(params.agent_radius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0F / cfg.cs);
    cfg.maxSimplificationError = 1.3F;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = 6.0F * cfg.cs;
    cfg.detailSampleMaxError = cfg.ch;
    rcVcopy(cfg.bmin, &bmin.x);
    rcVcopy(cfg.bmax, &bmax.x);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    // Terrain-scale input: cap the voxel grid (memory and bake time grow with
    // its square). Coarser cells on a 2 km world are the right trade — the
    // agent radius/climb stay in metres and re-derive from the new cell size.
    constexpr int kMaxGrid = 1024;
    if (cfg.width > kMaxGrid || cfg.height > kMaxGrid) {
        const float scale =
            static_cast<float>(std::max(cfg.width, cfg.height)) / static_cast<float>(kMaxGrid);
        cfg.cs *= scale;
        cfg.walkableRadius = static_cast<int>(std::ceil(params.agent_radius / cfg.cs));
        cfg.maxEdgeLen = static_cast<int>(12.0F / cfg.cs);
        cfg.detailSampleDist = 6.0F * cfg.cs;
        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    }
    // The inter-cell step test must agree with the slope limit at the chosen
    // cell size: neighbouring cells on a max-slope incline rise cs*tan(slope).
    // Without this, coarsened terrain bakes disconnect every hillside and only
    // flat ground survives (the per-triangle slope filter still rejects
    // anything steeper than the limit).
    const float slope_rise =
        cfg.cs * std::tan(params.agent_max_slope_deg * 3.14159265F / 180.0F);
    cfg.walkableClimb =
        std::max(cfg.walkableClimb, static_cast<int>(std::ceil(slope_rise / cfg.ch)));

    rcContext ctx(false);

    rcHeightfield* hf = rcAllocHeightfield();
    if (hf == nullptr ||
        !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                             cfg.ch)) {
        rcFreeHeightField(hf);
        return fail("navmesh: heightfield creation failed");
    }

    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, tris.data(), ntris,
                            areas.data());
    if (!rcRasterizeTriangles(&ctx, verts, nverts, tris.data(), areas.data(), ntris, *hf,
                              cfg.walkableClimb)) {
        rcFreeHeightField(hf);
        return fail("navmesh: rasterization failed");
    }

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    const bool compact_ok =
        chf != nullptr &&
        rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
    rcFreeHeightField(hf);
    if (!compact_ok) {
        rcFreeCompactHeightfield(chf);
        return fail("navmesh: compact heightfield failed");
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) ||
        !rcBuildDistanceField(&ctx, *chf) ||
        !rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        rcFreeCompactHeightfield(chf);
        return fail("navmesh: region build failed");
    }

    rcContourSet* cset = rcAllocContourSet();
    const bool contours_ok =
        cset != nullptr &&
        rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
    if (!contours_ok) {
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return fail("navmesh: contour build failed");
    }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    const bool poly_ok =
        pmesh != nullptr && rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);
    rcFreeContourSet(cset);
    if (!poly_ok || pmesh->npolys == 0) {
        rcFreePolyMesh(pmesh);
        rcFreeCompactHeightfield(chf);
        return fail("navmesh: polygon mesh build produced no polygons");
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    const bool detail_ok =
        dmesh != nullptr && rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist,
                                                  cfg.detailSampleMaxError, *dmesh);
    rcFreeCompactHeightfield(chf);
    if (!detail_ok) {
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        return fail("navmesh: detail mesh build failed");
    }

    // Every walkable poly gets flag 1 (the query filter includes everything).
    for (int i = 0; i < pmesh->npolys; ++i) {
        pmesh->flags[i] = 1;
    }

    dtNavMeshCreateParams np{};
    np.verts = pmesh->verts;
    np.vertCount = pmesh->nverts;
    np.polys = pmesh->polys;
    np.polyAreas = pmesh->areas;
    np.polyFlags = pmesh->flags;
    np.polyCount = pmesh->npolys;
    np.nvp = pmesh->nvp;
    np.detailMeshes = dmesh->meshes;
    np.detailVerts = dmesh->verts;
    np.detailVertsCount = dmesh->nverts;
    np.detailTris = dmesh->tris;
    np.detailTriCount = dmesh->ntris;
    np.walkableHeight = params.agent_height;
    np.walkableRadius = params.agent_radius;
    np.walkableClimb = params.agent_max_climb;
    rcVcopy(np.bmin, pmesh->bmin);
    rcVcopy(np.bmax, pmesh->bmax);
    np.cs = cfg.cs;
    np.ch = cfg.ch;
    np.buildBvTree = true;

    unsigned char* nav_data = nullptr;
    int nav_size = 0;
    const bool data_ok = dtCreateNavMeshData(&np, &nav_data, &nav_size);

    NavMesh out;
    out.impl_ = std::make_unique<Impl>();

    // Editor overlay: polygon edges in world space (quantized verts -> world).
    const glm::vec3 pb(pmesh->bmin[0], pmesh->bmin[1], pmesh->bmin[2]);
    const auto vert_at = [&](unsigned short idx) {
        const unsigned short* v = &pmesh->verts[static_cast<std::size_t>(idx) * 3];
        return pb + glm::vec3(static_cast<float>(v[0]) * cfg.cs,
                              static_cast<float>(v[1]) * cfg.ch,
                              static_cast<float>(v[2]) * cfg.cs);
    };
    for (int p = 0; p < pmesh->npolys; ++p) {
        const unsigned short* poly = &pmesh->polys[static_cast<std::size_t>(p) * pmesh->nvp * 2];
        for (int j = 0; j < pmesh->nvp; ++j) {
            if (poly[j] == RC_MESH_NULL_IDX) break;
            const int next = (j + 1 < pmesh->nvp && poly[j + 1] != RC_MESH_NULL_IDX) ? j + 1 : 0;
            out.impl_->edges.emplace_back(vert_at(poly[j]), vert_at(poly[next]));
        }
    }

    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);
    if (!data_ok) return fail("navmesh: dtCreateNavMeshData failed");

    dtNavMesh* mesh = dtAllocNavMesh();
    if (mesh == nullptr ||
        dtStatusFailed(mesh->init(nav_data, nav_size, DT_TILE_FREE_DATA))) {
        dtFree(nav_data);
        dtFreeNavMesh(mesh);
        return fail("navmesh: Detour init failed");
    }
    out.impl_->mesh.reset(mesh);

    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (query == nullptr || dtStatusFailed(query->init(mesh, 2048))) {
        dtFreeNavMeshQuery(query);
        return fail("navmesh: query init failed");
    }
    out.impl_->query.reset(query);
    return out;
}

std::expected<glm::vec3, core::Error> NavMesh::nearest_point(const glm::vec3& pos) const {
    if (!valid()) return fail("navmesh: not built");
    const dtQueryFilter filter;
    const float extents[3] = {4.0F, 8.0F, 4.0F};
    dtPolyRef ref = 0;
    float nearest[3]{};
    if (dtStatusFailed(
            impl_->query->findNearestPoly(&pos.x, extents, &filter, &ref, nearest)) ||
        ref == 0) {
        return fail("navmesh: position is off the mesh");
    }
    return glm::vec3(nearest[0], nearest[1], nearest[2]);
}

std::expected<std::vector<glm::vec3>, core::Error>
NavMesh::find_path(const glm::vec3& start, const glm::vec3& end) const {
    if (!valid()) return fail("navmesh: not built");
    const dtQueryFilter filter;
    const float extents[3] = {4.0F, 8.0F, 4.0F};

    dtPolyRef start_ref = 0;
    dtPolyRef end_ref = 0;
    float snapped_start[3]{};
    float snapped_end[3]{};
    impl_->query->findNearestPoly(&start.x, extents, &filter, &start_ref, snapped_start);
    impl_->query->findNearestPoly(&end.x, extents, &filter, &end_ref, snapped_end);
    if (start_ref == 0 || end_ref == 0) {
        return fail("navmesh: endpoint is off the mesh");
    }

    // Long terrain routes need room: a full corridor that exceeds this comes
    // back partial and the agent plans the next leg when it runs out.
    constexpr int max_polys = 1024;
    dtPolyRef polys[max_polys];
    int npolys = 0;
    if (dtStatusFailed(impl_->query->findPath(start_ref, end_ref, snapped_start, snapped_end,
                                              &filter, polys, &npolys, max_polys)) ||
        npolys == 0) {
        return fail("navmesh: no route between the endpoints");
    }

    float straight[max_polys * 3];
    int nstraight = 0;
    if (dtStatusFailed(impl_->query->findStraightPath(snapped_start, snapped_end, polys, npolys,
                                                      straight, nullptr, nullptr, &nstraight,
                                                      max_polys)) ||
        nstraight == 0) {
        return fail("navmesh: string-pull failed");
    }

    std::vector<glm::vec3> path;
    path.reserve(static_cast<std::size_t>(nstraight));
    for (int i = 0; i < nstraight; ++i) {
        path.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
    }
    return path;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error> run_navmesh_self_test() {
    // A 40x40 m plate with a wall across z = 0 leaving a gap on the +x side.
    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> idx;
    const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        verts.insert(verts.end(), {a, b, c, d});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad({-20.0F, 0.0F, -20.0F}, {-20.0F, 0.0F, 20.0F}, {20.0F, 0.0F, 20.0F},
         {20.0F, 0.0F, -20.0F});
    // Wall: a thin box from x in [-20, 12], 3 m tall, 1 m thick, centred on z=0.
    const float x0 = -20.0F, x1 = 12.0F, z0 = -0.5F, z1 = 0.5F, h = 3.0F;
    quad({x0, h, z0}, {x0, h, z1}, {x1, h, z1}, {x1, h, z0});           // top
    quad({x0, 0.0F, z0}, {x0, h, z0}, {x1, h, z0}, {x1, 0.0F, z0});     // -z face
    quad({x1, 0.0F, z1}, {x1, h, z1}, {x0, h, z1}, {x0, 0.0F, z1});     // +z face
    quad({x1, 0.0F, z0}, {x1, h, z0}, {x1, h, z1}, {x1, 0.0F, z1});     // +x cap

    auto mesh = NavMesh::build(verts, idx);
    if (!mesh) return std::unexpected(mesh.error());
    if (mesh->debug_edges().empty()) {
        return std::unexpected(core::Error{"navmesh self-test: no debug edges"});
    }

    auto path = mesh->find_path({-10.0F, 0.0F, -10.0F}, {-10.0F, 0.0F, 10.0F});
    if (!path) return std::unexpected(path.error());
    if (path->size() < 3) {
        return std::unexpected(
            core::Error{"navmesh self-test: path suspiciously direct through the wall"});
    }
    // The route must detour through the gap on the +x side and stay on the plate.
    float max_x = std::numeric_limits<float>::lowest();
    for (const glm::vec3& p : *path) {
        max_x = glm::max(max_x, p.x);
        if (p.y > 1.0F) {
            return std::unexpected(core::Error{"navmesh self-test: path climbed the wall"});
        }
    }
    if (max_x < 10.0F) {
        return std::unexpected(core::Error{"navmesh self-test: path did not use the gap"});
    }
    return {};
}

} // namespace engine::nav
