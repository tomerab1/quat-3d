# ---------------------------------------------------------------------------
# engine_pack + quat-pack — game packaging (ROADMAP P4): collects the files a
# game references (through the scene codec, glTF and Lua scanners), cooks
# them with quat-cook when available, stages the data root with a manifest,
# and drives scripts/package-release.sh for the per-OS layout. Tool-side only:
# never linked into the game or the player.
# ---------------------------------------------------------------------------
add_library(engine_pack STATIC
    digest.cpp
    lua_refs.cpp
    gltf_refs.cpp
    dependencies.cpp
    stage.cpp
    pack_cli.cpp)
target_link_libraries(engine_pack PUBLIC engine_flags engine_core engine_project engine_scene)
target_link_libraries(engine_pack PRIVATE OpenSSL::Crypto)
target_include_directories(engine_pack PUBLIC ${ENGINE_PUBLIC_INCLUDE})

add_executable(quat-pack ${PROJECT_SOURCE_DIR}/tools/quat-pack/main.cpp)
target_link_libraries(quat-pack PRIVATE engine_flags engine_pack)
set_target_properties(quat-pack PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
target_compile_definitions(quat-pack PRIVATE QUAT_VERSION="${PROJECT_VERSION}")
# Development builds find the cooked shaders and the layout scripts in the
# source tree; packaged builds take --shaders / --engine-root.
if(NOT ENGINE_PACKAGE_BUILD)
    target_compile_definitions(quat-pack PRIVATE
        QUAT_BUILD_TREE_ROOT="${PROJECT_SOURCE_DIR}"
        QUAT_BUILD_TREE_SHADER_DIR="${PROJECT_SOURCE_DIR}/cooked/shaders")
endif()
