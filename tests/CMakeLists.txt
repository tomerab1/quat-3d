# engine_tests — GoogleTest suite (ROADMAP S3).
#
# Layout: tests/<module>/*_test.cpp for test cases, tests/support/ for shared
# helpers (ASSERT_OK for std::expected, the surface-less GPU context),
# tests/legacy/ for the run_*_self_test() wrappers being migrated.
# GPU suites are named Gpu*; CTest runs CPU and GPU suites as separate tests
# so a missing device never hides CPU failures.

find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE ENGINE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)

# GameBoot (P4) is graphics-free application-layer logic: compiled in directly.
add_executable(engine_tests
    test_main.cpp
    support/alloc_probe.cpp
    support/gpu_context.cpp
    ${PROJECT_SOURCE_DIR}/src/app/game_boot.cpp
    ${PROJECT_SOURCE_DIR}/src/app/app_config.cpp
    ${ENGINE_TEST_SOURCES})
target_link_libraries(engine_tests PRIVATE
    engine_flags engine_editor_build engine_core engine_input engine_rhi engine_asset engine_scene
    engine_renderer engine_physics engine_animation engine_terrain engine_nav engine_net
    engine_remote engine_remote_world engine_audio engine_weather engine_geometry engine_script engine_cook
    engine_gltf_io game_world # gltf_io.hpp internals (fastgltf)
    engine_project engine_pack
    quat_asset_gen
    GTest::gtest)
# Full libktx in this program (engine_cook encodes; the tests decode ASTC
# references through it): engine_asset then links KTX::ktx instead of
# KTX::ktx_read, see the root CMakeLists. The tests also call libktx directly.
set_target_properties(engine_tests PROPERTIES QUAT_KTX_FULL ON)
target_include_directories(engine_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:KTX::ktx,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(engine_tests PRIVATE
    $<TARGET_PROPERTY:KTX::ktx,INTERFACE_COMPILE_DEFINITIONS>)
if(ENGINE_EDITOR)
    # imgui: the ImGui pass test builds its draw data in a private context.
    target_link_libraries(engine_tests PRIVATE engine_editor imgui::imgui)
endif()
target_include_directories(engine_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${ENGINE_PUBLIC_INCLUDE})
# Image round trips (stb_image) in the screenshot tests; SYSTEM so MSVC /W4
# does not warn inside stb.
target_include_directories(engine_tests SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# White-box script VM tests (tests/script/vm_test.cpp) include engine_script's
# private headers, which use sol2 and Lua.
target_link_libraries(engine_tests PRIVATE sol2::sol2 ${LUA_LIBRARIES})
target_include_directories(engine_tests SYSTEM PRIVATE ${LUA_INCLUDE_DIR})
target_compile_definitions(engine_tests PRIVATE SOL_NO_EXCEPTIONS=1 SOL_ALL_SAFETIES_ON=1)
target_compile_definitions(engine_tests PRIVATE
    QUAT_COOKED_SHADER_DIR="${PROJECT_SOURCE_DIR}/cooked/shaders"
    QUAT_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/assets/shaders"
    # Shipped / vendored assets: the G7 demo scripts, editor fonts, icon table.
    QUAT_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
add_dependencies(engine_tests shaders)

# The geometry tests compare generated meshes with golden hashes that must
# hold on every platform, so the inputs they compute follow engine_geometry's
# floating-point rules too (no contraction; root CMakeLists.txt).
file(GLOB ENGINE_GEOMETRY_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/geometry/*_test.cpp)
if(MSVC)
    set_source_files_properties(${ENGINE_GEOMETRY_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS /fp:precise)
else()
    set_source_files_properties(${ENGINE_GEOMETRY_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

add_test(NAME engine_tests_cpu COMMAND engine_tests "--gtest_filter=-Gpu*")
add_test(NAME engine_tests_gpu COMMAND engine_tests "--gtest_filter=Gpu*" --require-gpu)
set_tests_properties(engine_tests_gpu PROPERTIES LABELS gpu)

# Binary-content checks on the executables. GCC/Clang: the symbol table via
# nm. MSVC: release executables carry no COFF symbols (dumpbin /symbols lists
# nothing), so the checked executables write linker maps (root CMakeLists) and
# the checks read those.
#  - game, quat-player-shipping: link the read-only libktx, no Basis / ASTC
#    encoder code.
#  - quat-player-shipping: contains no remote-control code (P4).
if(MSVC)
    add_test(NAME game_has_no_texture_encoder
        COMMAND ${CMAKE_COMMAND} -DMAP=$<TARGET_FILE_DIR:game>/game.map
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoTextureEncoder.cmake)
    add_test(NAME quat_player_shipping_has_no_texture_encoder
        COMMAND ${CMAKE_COMMAND}
                -DMAP=$<TARGET_FILE_DIR:quat-player-shipping>/quat-player-shipping.map
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoTextureEncoder.cmake)
    add_test(NAME quat_player_shipping_has_no_remote
        COMMAND ${CMAKE_COMMAND}
                -DMAP=$<TARGET_FILE_DIR:quat-player-shipping>/quat-player-shipping.map
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoRemoteControl.cmake)
elseif(CMAKE_NM)
    add_test(NAME game_has_no_texture_encoder
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:game>
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoTextureEncoder.cmake)
    add_test(NAME quat_player_shipping_has_no_texture_encoder
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:quat-player-shipping>
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoTextureEncoder.cmake)
    add_test(NAME quat_player_shipping_has_no_remote
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:quat-player-shipping>
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoRemoteControl.cmake)
else()
    message(WARNING "binary-content checks (no texture encoder, no remote control): no nm found; not registered")
endif()

# Game packages carry the engine's LICENSE and THIRD_PARTY_NOTICES.md (#122):
# scripts/package-release.sh with a real player (Linux: the layout under test
# runs ldd / patchelf on it). Skipped (77) without patchelf.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(QUAT_BASH_FOR_TESTS bash)
    if(QUAT_BASH_FOR_TESTS)
        add_test(NAME package_licences
            COMMAND ${QUAT_BASH_FOR_TESTS} ${PROJECT_SOURCE_DIR}/tests/pack/package_licences_test.sh
                    ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR} $<TARGET_FILE:quat-player-shipping>)
        set_tests_properties(package_licences PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# Engine SDK guards: install, self-contained public headers, the SDK
# consumer, refusals, layering (ROADMAP P8).
add_subdirectory(sdk)
