# ---------------------------------------------------------------------------
# game — executable entry point. The full target graph (engine_core /
# engine_renderer / engine_editor) grows phase by phase as modules gain sources.
# ---------------------------------------------------------------------------
# src/app/ is the application layer (Application owns the window, device and
# frame loop; FrameRenderer, WorldRuntime, FlyCamera, EditorBridge, the remote
# bridge and the startup scenes run inside it); it is not an engine library.
# Two executables share it (P4): `game` (the editor build when ENGINE_EDITOR is
# on) and `quat-player`, the runtime player games ship with — always built
# without the editor. Each entry point only parses the environment and hands
# off to Application.
#
# game_world holds the window-free part of the app layer — WorldRuntime (the
# simulated world: demo scenes, terrain, grass, particles, navmesh, audio,
# play mode and gameplay scripts) — so tests can drive the exact frame-loop
# simulation path (tests/app/, including the play state machine) without SDL.
# world_runtime.cpp has editor-only behaviour (script hot reload on, the play
# session's physics world), so an editor build compiles it twice: game_world
# with ENGINE_EDITOR for `game`, game_world_runtime without it for the players.
set(GAME_WORLD_SOURCES
    world_runtime.cpp
    demo_scenes.cpp
    audio_runtime.cpp
    play_mode.cpp
    grass_runtime.cpp
    particle_runtime.cpp
    nav_bake.cpp
    input_runtime.cpp)
set(GAME_WORLD_LIBRARIES
    engine_flags engine_core engine_input engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_audio engine_script)
add_library(game_world STATIC ${GAME_WORLD_SOURCES})
target_link_libraries(game_world PUBLIC ${GAME_WORLD_LIBRARIES})
target_link_libraries(game_world PRIVATE engine_editor_build)
target_include_directories(game_world PUBLIC
    ${ENGINE_PUBLIC_INCLUDE}
    ${PROJECT_SOURCE_DIR}/src)
if(ENGINE_EDITOR)
    add_library(game_world_runtime STATIC ${GAME_WORLD_SOURCES})
    target_link_libraries(game_world_runtime PUBLIC ${GAME_WORLD_LIBRARIES})
    target_include_directories(game_world_runtime PUBLIC
        ${ENGINE_PUBLIC_INCLUDE}
        ${PROJECT_SOURCE_DIR}/src)
else()
    add_library(game_world_runtime ALIAS game_world)
endif()

set(QUAT_APP_SOURCES
    application.cpp
    app_config.cpp
    build_service.cpp
    fly_camera.cpp
    frame_graph.cpp
    frame_temporal.cpp
    frame_post.cpp
    frame_environment.cpp
    frame_renderer.cpp
    frame_profiling.cpp
    game_boot.cpp
    game_startup.cpp
    input_sampler.cpp
    remote_bridge.cpp
    screenshot_capture.cpp
    window_icon.cpp
    asset_root.cpp)
set(QUAT_APP_LIBRARIES
    engine_flags engine_core engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_remote engine_remote_world engine_audio
    engine_script engine_project SDL3::SDL3)

add_executable(game ${PROJECT_SOURCE_DIR}/src/main.cpp ${QUAT_APP_SOURCES})
target_link_libraries(game PRIVATE game_world ${QUAT_APP_LIBRARIES} engine_editor_build)
if(MSVC)
    # Linker map for the game_has_no_texture_encoder test.
    target_link_options(game PRIVATE "/MAP:$<TARGET_FILE_DIR:game>/game.map")
endif()
if(ENGINE_EDITOR)
    target_sources(game PRIVATE editor_bridge.cpp)
    target_link_libraries(game PRIVATE engine_editor)
endif()

add_executable(quat-player ${PROJECT_SOURCE_DIR}/src/player/main.cpp ${QUAT_APP_SOURCES})
target_link_libraries(quat-player PRIVATE game_world_runtime ${QUAT_APP_LIBRARIES})

# quat-player-shipping: what `quat-pack --config shipping` ships. Built with
# QUAT_SHIPPING, which compiles remote control out (no endpoint, no remote
# screenshots, no QUAT_GAME override): it does not link engine_remote /
# engine_remote_world at all, so any remote reference left in the shipping
# path fails the link, and the quat_player_shipping_has_no_remote test checks
# the symbol table.
set(QUAT_SHIPPING_APP_SOURCES ${QUAT_APP_SOURCES})
list(REMOVE_ITEM QUAT_SHIPPING_APP_SOURCES
    screenshot_capture.cpp # remote render.screenshot only
    build_service.cpp)     # project.build / the editor's export only
set(QUAT_SHIPPING_APP_LIBRARIES ${QUAT_APP_LIBRARIES})
list(REMOVE_ITEM QUAT_SHIPPING_APP_LIBRARIES engine_remote engine_remote_world)
add_executable(quat-player-shipping ${PROJECT_SOURCE_DIR}/src/player/main.cpp ${QUAT_SHIPPING_APP_SOURCES})
target_link_libraries(quat-player-shipping PRIVATE game_world_runtime ${QUAT_SHIPPING_APP_LIBRARIES})
target_compile_definitions(quat-player-shipping PRIVATE QUAT_SHIPPING)
if(MSVC)
    # Linker map for the quat_player_shipping_has_no_remote test.
    target_link_options(quat-player-shipping PRIVATE
        "/MAP:$<TARGET_FILE_DIR:quat-player-shipping>/quat-player-shipping.map")
endif()

# A game double-clicked on Windows opens no console window (wWinMain entry).
set_target_properties(quat-player quat-player-shipping PROPERTIES WIN32_EXECUTABLE TRUE)
# The executables stay at the top of the build tree, where scripts, CI and
# the docs run them (build/<preset>/game), whichever module defines them.
set_target_properties(game quat-player quat-player-shipping PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

foreach(app_target game quat-player quat-player-shipping)
    target_include_directories(${app_target} PRIVATE
        ${ENGINE_PUBLIC_INCLUDE}
        ${PROJECT_SOURCE_DIR}/src)
    # stb_image decodes the window icon (the implementation is in engine_scene).
    target_include_directories(${app_target} SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
endforeach()
# Data root, resolved at runtime (src/app/asset_root.hpp): QUAT_ASSET_ROOT, the
# executable's directory (packaged builds), then - development builds only -
# the source tree baked in here.
if(NOT ENGINE_PACKAGE_BUILD)
    set_property(SOURCE asset_root.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        QUAT_BUILD_TREE_ROOT="${PROJECT_SOURCE_DIR}"
        QUAT_BUILD_TREE_SHADER_DIR="${PROJECT_SOURCE_DIR}/cooked/shaders")
endif()
