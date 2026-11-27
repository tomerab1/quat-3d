# quat-asset-gen: procedural, license-clean nature models (conifers, broadleaf
# trees, rocks, a bush) written as .glb. The library is linked by engine_tests
# (determinism + loader validation); the executable regenerates the committed
# files: quat-asset-gen assets/meshes/nature
add_library(quat_asset_gen STATIC
    mesh_builder.cpp
    glb_writer.cpp
    nature_models.cpp)
target_link_libraries(quat_asset_gen PUBLIC engine_flags glm::glm nlohmann_json::nlohmann_json)
target_include_directories(quat_asset_gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(quat-asset-gen main.cpp)
target_link_libraries(quat-asset-gen PRIVATE quat_asset_gen)
