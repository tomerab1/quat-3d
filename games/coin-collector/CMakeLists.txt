# Coin Collector — the in-tree sample game (ROADMAP P4; a native game module
# since G17.1). A leaf of this repository: nothing in the engine refers to it
# (layering_check). It is built like a game in its own repository, through
# the SDK's Quat3dGame.cmake functions and the quat3d:: targets only:
#   coin-collector            the player (quat-pack --player)
#   coin-collector-shipping   the shipping player
#   coin-collector-editor     the editor with the module (editor builds)
quat_add_game_module(coin_collector_module
    SOURCES module/coin_module.cpp
    REGISTER coin_collector::register_module)
target_include_directories(coin_collector_module PUBLIC module)

quat_add_game(coin-collector
    PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    MODULES coin_collector_module)

find_package(GTest CONFIG REQUIRED)
add_executable(coin_collector_tests tests/coin_module_test.cpp)
target_link_libraries(coin_collector_tests PRIVATE coin_collector_module GTest::gtest GTest::gtest_main)
target_compile_definitions(coin_collector_tests PRIVATE
    COIN_COLLECTOR_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
quat_copy_runtime_dlls(coin_collector_tests)
add_test(NAME coin_collector_module COMMAND coin_collector_tests)
