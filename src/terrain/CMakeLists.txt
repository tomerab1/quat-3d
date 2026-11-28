# ---------------------------------------------------------------------------
# engine_terrain — procedural terrain (Phase 12). Pure CPU heightmap generation
# in 12.1; chunked LOD rendering and Jolt height fields layer on in 12.2/12.3.
# ---------------------------------------------------------------------------
add_library(engine_terrain STATIC
    budget.cpp
    cpu_tile_jobs.cpp
    generator.cpp
    grid_erosion.cpp
    streamer.cpp
    talus_relax.cpp)
target_link_libraries(engine_terrain PUBLIC engine_flags glm::glm)
target_link_libraries(engine_terrain PRIVATE engine_core) # profiler zones + thread names (R4)
target_include_directories(engine_terrain PUBLIC ${ENGINE_PUBLIC_INCLUDE})
