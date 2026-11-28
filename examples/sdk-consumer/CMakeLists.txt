# sdk-consumer — a tiny game built ONLY against the installed quat-3d SDK
# (ROADMAP P8, docs/sdk.md, docs/design/cozy-builder.md §3.5). It never sees
# the engine's source tree: a header that leaks a private include, a missing
# export, an uninstalled dependency or a changed signature fails here, in the
# engine PR that caused it.
#
#   cmake --install build/release --prefix <sdk>
#   cmake -S examples/sdk-consumer -B build-consumer -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=<sdk>/lib/cmake/quat3d/quat3d-toolchain.cmake
#   cmake --build build-consumer && ctest --test-dir build-consumer
#
# The engine's own CTest `sdk_consumer` runs exactly these steps.
cmake_minimum_required(VERSION 3.22)
project(sdk_consumer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The SDK version this game is written against; 0.x accepts only 0.1.y.
find_package(quat3d 0.1 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED) # from the SDK's bundled deps/

# The game's native code, as a static library the game and its tests share.
add_library(sdk_consumer_module STATIC src/spinner.cpp)
target_include_directories(sdk_consumer_module PUBLIC src)
target_link_libraries(sdk_consumer_module PUBLIC quat3d::engine quat3d::remote)
if(MSVC)
    target_compile_options(sdk_consumer_module PRIVATE /W4 /WX)
else()
    target_compile_options(sdk_consumer_module PRIVATE -Wall -Wextra -Wshadow -Werror)
endif()

# The native module (ROADMAP G17.1) and the game's own players built from
# it: sdk_consumer_player (+ -shipping, -editor) with a generated main().
quat_add_game_module(sdk_consumer_game_module
    SOURCES src/spinner_module.cpp
    REGISTER consumer::register_module
    NAME sdk_consumer)
target_link_libraries(sdk_consumer_game_module PUBLIC sdk_consumer_module)
quat_add_game(sdk_consumer_player
    PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    ASSETS assets
    MODULES sdk_consumer_game_module)

quat_add_game(sdk_consumer_game
    SOURCES src/main.cpp
    ASSETS assets
    LIBRARIES sdk_consumer_module
    REMOTE)

enable_testing()
add_executable(sdk_consumer_tests tests/spinner_test.cpp)
target_link_libraries(sdk_consumer_tests PRIVATE sdk_consumer_game_module GTest::gtest GTest::gtest_main)
quat_copy_runtime_dlls(sdk_consumer_tests) # Windows: the SDK's dependency DLLs
add_test(NAME sdk_consumer_tests COMMAND sdk_consumer_tests)
# The game runs headless: it checks its data root and steps its world.
add_test(NAME sdk_consumer_game COMMAND sdk_consumer_game --frames 120)
