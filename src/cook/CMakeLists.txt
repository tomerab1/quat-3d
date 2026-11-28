# ---------------------------------------------------------------------------
# engine_cook — offline asset cooker (R17): glTF textures -> KTX2/Basis,
# meshes -> meshoptimizer passes, quantisation and EXT_meshopt_compression.
# Used by tools/quat-cook and the tests; never linked into the game.
# ---------------------------------------------------------------------------
add_library(engine_cook STATIC
    gltf_access.cpp
    gltf_cooker.cpp
    mesh_cook.cpp
    mesh_simplify.cpp
    mesh_streams.cpp
    texture_encode.cpp)
target_link_libraries(engine_cook PUBLIC engine_flags)
target_link_libraries(engine_cook PRIVATE
    engine_image engine_gltf_io fastgltf::fastgltf KTX::ktx meshoptimizer::meshoptimizer)
target_include_directories(engine_cook PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_cook PRIVATE ${PROJECT_SOURCE_DIR}/src)

add_executable(quat-cook ${PROJECT_SOURCE_DIR}/tools/quat-cook/main.cpp)
target_link_libraries(quat-cook PRIVATE engine_flags engine_cook)
# Full libktx (engine_cook encodes); see engine_asset.
set_target_properties(quat-cook PROPERTIES QUAT_KTX_FULL ON)
set_target_properties(quat-cook PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
