# ---------------------------------------------------------------------------
# game — executable entry point. The full target graph (engine_core /
# engine_renderer / engine_editor) grows phase by phase as modules gain sources.
# ---------------------------------------------------------------------------
# src/app/ is the application layer (Application owns the window, device and
# frame loop; FrameRenderer, WorldRuntime, FlyCamera, EditorBridge, the remote
# bridge and the startup scenes run inside it). It is built as the player
# libraries below; the executables (P4: `game`, the editor build when
# ENGINE_EDITOR is on, and `quat-player`, the runtime player games ship with,
# always built without the editor) only parse the environment and hand off to
# Application.
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
    module_runtime.cpp
    demo_scenes.cpp
    audio_runtime.cpp
    play_mode.cpp
    grass_runtime.cpp
    particle_runtime.cpp
    nav_bake.cpp
    input_runtime.cpp)
set(GAME_WORLD_LIBRARIES
    engine_flags engine_core engine_input engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_audio engine_script engine_game)
# src/ is a build-tree include path only: the SDK exports these libraries for
# games' own players (G17.1) but installs no private header.
add_library(game_world STATIC ${GAME_WORLD_SOURCES})
target_link_libraries(game_world PUBLIC ${GAME_WORLD_LIBRARIES})
target_link_libraries(game_world PRIVATE engine_editor_build)
target_include_directories(game_world PUBLIC
    ${ENGINE_PUBLIC_INCLUDE}
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)
if(ENGINE_EDITOR)
    add_library(game_world_runtime STATIC ${GAME_WORLD_SOURCES})
    target_link_libraries(game_world_runtime PUBLIC ${GAME_WORLD_LIBRARIES})
    target_include_directories(game_world_runtime PUBLIC
        ${ENGINE_PUBLIC_INCLUDE}
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)
    set(QUAT_WORLD_RUNTIME game_world_runtime)
else()
    set(QUAT_WORLD_RUNTIME game_world) # no editor: one world library serves all
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
    module_methods.cpp
    remote_bridge.cpp
    run_game.cpp
    screenshot_capture.cpp
    window_icon.cpp
    asset_root.cpp)
set(QUAT_APP_LIBRARIES
    engine_flags engine_core engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_remote engine_remote_world engine_audio
    engine_script engine_project engine_game SDL3::SDL3)

# The player libraries (G17.1): the application layer, compiled once per
# configuration, behind engine::game::run_player / run_editor
# (engine/game/player.hpp). The engine's own executables are thin mains over
# them, and the SDK exports them so a game's quat_add_game() builds the same
# players with the game's modules:
#   engine_player           quat-player            quat3d::player
#   engine_player_shipping  quat-player-shipping   quat3d::player_shipping
#   engine_player_editor    game (ENGINE_EDITOR)   quat3d::player_editor
# quat-player-shipping (what `quat-pack --config shipping` ships) is built
# with QUAT_SHIPPING, which compiles remote control out (no endpoint, no
# remote screenshots, no QUAT_GAME override): its library does not link
# engine_remote / engine_remote_world at all, so any remote reference left in
# the shipping path fails the link, and the quat_player_shipping_has_no_remote
# test checks the symbol table.
function(quat_app_library target)
    cmake_parse_arguments(ARG "" "WORLD" "SOURCES;LIBRARIES" ${ARGN})
    add_library(${target} STATIC ${ARG_SOURCES})
    target_link_libraries(${target} PUBLIC ${ARG_WORLD} ${ARG_LIBRARIES})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_include_directories(${target} PUBLIC ${ENGINE_PUBLIC_INCLUDE})
    # stb_image decodes the window icon (the implementation is in engine_scene).
    target_include_directories(${target} SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
endfunction()

quat_app_library(engine_player
    WORLD ${QUAT_WORLD_RUNTIME} SOURCES ${QUAT_APP_SOURCES} LIBRARIES ${QUAT_APP_LIBRARIES})

set(QUAT_SHIPPING_APP_SOURCES ${QUAT_APP_SOURCES})
list(REMOVE_ITEM QUAT_SHIPPING_APP_SOURCES
    screenshot_capture.cpp # remote render.screenshot only
    build_service.cpp      # project.build / the editor's export only
    module_methods.cpp)    # module methods on the remote endpoint
set(QUAT_SHIPPING_APP_LIBRARIES ${QUAT_APP_LIBRARIES})
list(REMOVE_ITEM QUAT_SHIPPING_APP_LIBRARIES engine_remote engine_remote_world)
quat_app_library(engine_player_shipping
    WORLD ${QUAT_WORLD_RUNTIME} SOURCES ${QUAT_SHIPPING_APP_SOURCES} LIBRARIES ${QUAT_SHIPPING_APP_LIBRARIES})
target_compile_definitions(engine_player_shipping PRIVATE QUAT_SHIPPING)

if(ENGINE_EDITOR)
    quat_app_library(engine_player_editor
        WORLD game_world SOURCES ${QUAT_APP_SOURCES} editor_bridge.cpp
        LIBRARIES ${QUAT_APP_LIBRARIES} engine_editor)
    target_link_libraries(engine_player_editor PRIVATE engine_editor_build)
    set(QUAT_GAME_APP engine_player_editor)
else()
    set(QUAT_GAME_APP engine_player)
endif()

add_executable(game ${PROJECT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(game PRIVATE ${QUAT_GAME_APP} engine_editor_build)
if(MSVC)
    # Linker map for the game_has_no_texture_encoder test.
    target_link_options(game PRIVATE "/MAP:$<TARGET_FILE_DIR:game>/game.map")
endif()

add_executable(quat-player ${PROJECT_SOURCE_DIR}/src/player/main.cpp)
target_link_libraries(quat-player PRIVATE engine_player)

add_executable(quat-player-shipping ${PROJECT_SOURCE_DIR}/src/player/main.cpp)
target_link_libraries(quat-player-shipping PRIVATE engine_player_shipping)
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
endforeach()
# Data root, resolved at runtime (src/app/asset_root.hpp): QUAT_ASSET_ROOT, the
# executable's directory (packaged builds), then - development builds only -
# the source tree baked in here.
if(NOT ENGINE_PACKAGE_BUILD)
    set_property(SOURCE asset_root.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        QUAT_BUILD_TREE_ROOT="${PROJECT_SOURCE_DIR}"
        QUAT_BUILD_TREE_SHADER_DIR="${PROJECT_SOURCE_DIR}/cooked/shaders")
endif()
