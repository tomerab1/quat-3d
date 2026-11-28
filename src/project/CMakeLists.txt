# ---------------------------------------------------------------------------
# engine_project — game projects (ROADMAP P4): the game.json descriptor and
# its validation, the package manifest, and BuildJob (quat-pack run in the
# background for the editor's export dialog and remote project.build).
# Graphics-free.
# ---------------------------------------------------------------------------
add_library(engine_project STATIC
    game_project.cpp
    package_manifest.cpp
    build_job.cpp)
target_link_libraries(engine_project PUBLIC engine_flags engine_core nlohmann_json::nlohmann_json)
target_link_libraries(engine_project PRIVATE glm::glm Threads::Threads)
target_include_directories(engine_project PUBLIC ${ENGINE_PUBLIC_INCLUDE})
