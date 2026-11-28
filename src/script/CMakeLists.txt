# ---------------------------------------------------------------------------
# engine_script — Lua gameplay scripting (G7): one sandboxed Lua 5.4 state
# per play session with per-call instruction / memory limits, the Script
# component's runtime, hot reload, and the engine API scripts call (entities,
# components through the codec, physics, input, audio, navigation). Lua and
# sol2 are implementation details (PRIVATE); the public headers are Lua-free.
# sol2 runs without exceptions (the engine builds with -fno-exceptions) and
# with every safety check on.
# ---------------------------------------------------------------------------
add_library(engine_script STATIC
    lua_vm.cpp
    lua_bind.cpp
    lua_json.cpp
    script_api_math.cpp
    script_api_entity.cpp
    script_api_scene.cpp
    script_api_services.cpp
    script_api_input.cpp
    script_source.cpp
    script_runtime.cpp
    script_files.cpp)
target_link_libraries(engine_script PUBLIC engine_flags engine_core engine_input engine_scene engine_physics)
target_link_libraries(engine_script PRIVATE sol2::sol2 quat3d_deps::lua)
# SYSTEM: the vcpkg include directory holds every dependency's headers.
target_include_directories(engine_script SYSTEM PRIVATE ${LUA_INCLUDE_DIR})
target_include_directories(engine_script PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_compile_definitions(engine_script PRIVATE SOL_NO_EXCEPTIONS=1 SOL_ALL_SAFETIES_ON=1)
