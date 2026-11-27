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

add_executable(engine_tests
    test_main.cpp
    support/gpu_context.cpp
    ${ENGINE_TEST_SOURCES})
target_link_libraries(engine_tests PRIVATE
    engine_flags engine_core engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_animation engine_terrain engine_nav engine_net engine_remote engine_remote_world
    engine_audio engine_cook
    GTest::gtest
    # Full libktx: engine_asset's KTX2 loader plus the cooker's encoder and
    # the ASTC reference decoder the tests use (see the root CMakeLists).
    KTX::ktx)
if(ENGINE_EDITOR)
    # imgui: the ImGui pass test builds its draw data in a private context.
    target_link_libraries(engine_tests PRIVATE engine_editor imgui::imgui)
endif()
target_include_directories(engine_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${ENGINE_PUBLIC_INCLUDE}
    ${Stb_INCLUDE_DIR}) # image round trips (stb_image) in the screenshot tests
target_compile_definitions(engine_tests PRIVATE
    QUAT_COOKED_SHADER_DIR="${PROJECT_SOURCE_DIR}/cooked/shaders")
add_dependencies(engine_tests shaders)

add_test(NAME engine_tests_cpu COMMAND engine_tests "--gtest_filter=-Gpu*")
add_test(NAME engine_tests_gpu COMMAND engine_tests "--gtest_filter=Gpu*" --require-gpu)
set_tests_properties(engine_tests_gpu PROPERTIES LABELS gpu)

# The game must link the read-only libktx: no Basis / ASTC encoder code.
if(NOT MSVC AND CMAKE_NM)
    add_test(NAME game_has_no_texture_encoder
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:game>
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoTextureEncoder.cmake)
endif()
