# quat-asset-gen: procedural, license-clean nature models (conifers, broadleaf
# trees, rocks, a bush) written as .glb. Neither target is in the default
# build: engine_tests links the library (determinism + loader validity), and
# the tool is built on request to regenerate the committed files:
#   cmake --build --preset release --target quat-asset-gen
#   ./build/release/tools/asset-gen/quat-asset-gen assets/meshes/nature
add_library(quat_asset_gen STATIC EXCLUDE_FROM_ALL
    mesh_builder.cpp
    glb_writer.cpp
    nature_models.cpp)
target_link_libraries(quat_asset_gen PUBLIC engine_flags glm::glm nlohmann_json::nlohmann_json)
target_include_directories(quat_asset_gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(quat-asset-gen EXCLUDE_FROM_ALL main.cpp)
target_link_libraries(quat-asset-gen PRIVATE quat_asset_gen)
# engine/core/utf8_path.hpp (header-only): paths printed as UTF-8.
target_include_directories(quat-asset-gen PRIVATE ${PROJECT_SOURCE_DIR}/include)
