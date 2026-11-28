# ---------------------------------------------------------------------------
# engine_scene — scene-construction side of the asset pipeline. For now just the
# glTF loader (fastgltf -> MeshAsset); gains the ECS world + systems in Phase 4.
# fastgltf is an implementation detail of the loader (PRIVATE).
# ---------------------------------------------------------------------------
add_library(engine_scene STATIC
    gltf/mesh.cpp
    gltf/material.cpp
    gltf/skin_animation.cpp
    gltf/clip_compression.cpp
    gltf/scene_assembly.cpp
    gltf/self_tests.cpp
    gltf/texture.cpp
    components.cpp
    scene.cpp
    animation_system.cpp
    physics_system.cpp
    joint_system.cpp
    nav_agent_system.cpp
    nav_separation.cpp
    navigation.cpp
    behavior.cpp
    bt_graph.cpp
    bt_graph_edit.cpp
    scene_io.cpp
    scene_references.cpp
    component_json.cpp
    component_json_common.cpp
    component_registry.cpp
    lens_json.cpp
    particle_json.cpp
    script_component.cpp
    scene_snapshot.cpp
    component_json_animation.cpp
    material_override.cpp
    render_settings_json.cpp
    physics_settings.cpp
    weather_json.cpp
    weather_frame.cpp
    audio_system.cpp
    primitives.cpp
    primitive_resolve.cpp
    scatter.cpp
    instanced_mesh.cpp
    instanced_mesh_resolve.cpp
    ground_placement.cpp
    camera_framing.cpp
    camera_rig.cpp
    camera_rig_system.cpp
    component_json_camera_rig.cpp
    world_clock.cpp
    world_clock_json.cpp
    world_clock_api.cpp
    bounds.cpp
    view_clip.cpp)
# EnTT is PUBLIC: components.hpp exposes entt types (Parent/Children) to consumers.
# engine_renderer is PUBLIC: scene.hpp's RenderCollectSystem fills a renderer
# DrawItem list (no cycle — the renderer does not depend on the scene).
# engine_animation is PUBLIC: components.hpp's SkinnedMesh holds a SkeletonAsset
# handle and the glTF loader extracts skins.
target_link_libraries(engine_scene PUBLIC
    engine_flags engine_input engine_asset engine_rhi engine_renderer engine_animation engine_physics
    engine_terrain engine_nav engine_audio engine_weather engine_astro EnTT::EnTT)
# fastgltf and stb (image decode) are implementation details of the loader.
# nlohmann_json is PUBLIC: component_json.hpp exposes JSON values (the codec
# shared by scene files and the remote-control API).
target_link_libraries(engine_scene PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(engine_scene PRIVATE fastgltf::fastgltf engine_gltf_io)
target_include_directories(engine_scene SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(engine_scene PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(engine_scene PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# Codec components (ROADMAP S9): one file per component in codec/, each
# defining engine::scene::codec::register_<file stem>(ComponentRegistry&). The
# call list is generated from the directory: a new component is a new file.
quat_generate_registration(engine_scene
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codec
    PATTERN   "*.cpp"
    TEMPLATE  ${CMAKE_CURRENT_SOURCE_DIR}/engine_components.cpp.in
    OUTPUT    ${CMAKE_CURRENT_BINARY_DIR}/generated/engine_components.cpp
    DECLARE   "[[nodiscard]] std::expected<void, core::Error> register_@NAME@(ComponentRegistry& registry)"
    CALL      "    keep_first(codec::register_@NAME@(registry))")

# engine_gltf_io — glTF parse + validation, EXT_meshopt_compression decoding
# and buffer access (src/scene/gltf/gltf_io.hpp): fastgltf only, no ECS or
# GPU, so the cooker parses and validates exactly like the runtime loader.
add_library(engine_gltf_io STATIC
    gltf/validate.cpp
    gltf/meshopt_decode.cpp
    gltf/gltf_source.cpp)
target_link_libraries(engine_gltf_io PUBLIC engine_flags engine_image fastgltf::fastgltf)
target_link_libraries(engine_gltf_io PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(engine_gltf_io PUBLIC
    ${ENGINE_PUBLIC_INCLUDE} $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)

# stb_image's implementation (PNG + JPEG only), used through engine_image's
# guarded decoder. Its TU is not warning-clean under our strict flags.
add_library(engine_stb_image STATIC stb_image_impl.cpp)
target_link_libraries(engine_stb_image PRIVATE engine_flags)
target_include_directories(engine_stb_image SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
set_source_files_properties(stb_image_impl.cpp PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
