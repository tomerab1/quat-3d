# ---------------------------------------------------------------------------
# engine_core — graphics-free foundations: the log ring + log() helper, stderr
# capture into the ring, the simulation clock (A5; S6 grows the logging layer
# here), file watching and child processes (shader hot reload, R2), and the
# runtime data-root resolution used by the game and packaged builds (P4).
# ---------------------------------------------------------------------------
add_library(engine_core STATIC
    log.cpp
    stderr_capture.cpp
    sim_clock.cpp
    event_ring.cpp
    frame_timer.cpp
    path_confine.cpp
    file_watcher.cpp
    child_process.cpp
    asset_root.cpp
    path_confine.cpp
    env.cpp
    profile_names.cpp
    profile_stats.cpp
    profile_capture.cpp
    frame_profiler.cpp
    version.cpp)
# glm + nlohmann_json are PUBLIC: engine/core/json_fields.hpp (header-only,
# part of the SDK's stable API) reads JSON into glm types.
target_link_libraries(engine_core PUBLIC engine_flags Threads::Threads
    glm::glm nlohmann_json::nlohmann_json)
target_include_directories(engine_core PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# QUAT_TRACY switches engine/core/profile.hpp's macros (and the GPU zones of
# rhi::GpuProfiler) to Tracy as well; PUBLIC so every engine target that
# instruments code sees the same configuration.
if(ENGINE_PROFILE)
    target_link_libraries(engine_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(engine_core PUBLIC QUAT_TRACY)
endif()
