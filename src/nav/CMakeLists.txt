# ---------------------------------------------------------------------------
# engine_nav — Recast/Detour navigation (Phase 13). Recast is an implementation
# detail (PRIVATE); the public header is Recast-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_nav STATIC
    navmesh.cpp)
target_link_libraries(engine_nav PUBLIC engine_flags glm::glm)
target_link_libraries(engine_nav PRIVATE
    RecastNavigation::Recast RecastNavigation::Detour RecastNavigation::DetourCrowd)
target_include_directories(engine_nav PUBLIC ${ENGINE_PUBLIC_INCLUDE})
