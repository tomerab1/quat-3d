# ---------------------------------------------------------------------------
# engine_remote_world — the world commands (ROADMAP A2/A3): scene.*,
# environment.*, camera.*, render.screenshot, asset.*, sim.*, nav.bake, debug.*
# and bt.* (behaviour-tree authoring), on top of engine_remote + the scene
# codec and the behaviour-tree graph model. Application-only capabilities come in
# through WorldServices callbacks (src/app/remote_bridge.cpp wires them), so
# the commands are testable on a bare Scene.
# ---------------------------------------------------------------------------
add_library(engine_remote_world STATIC
    common.cpp
    scene_commands.cpp
    batch_command.cpp
    environment_commands.cpp
    terrain_commands.cpp
    surface_query.cpp
    placement_commands.cpp
    camera_commands.cpp
    app_commands.cpp
    debug_commands.cpp
    debug_query_commands.cpp
    shader_commands.cpp
    tuning_commands.cpp
    behavior_commands.cpp
    behavior_edit_commands.cpp
    physics_commands.cpp
    physics_body_commands.cpp
    physics_query_commands.cpp
    weather_commands.cpp
    project_commands.cpp
    time_commands.cpp
    script_commands.cpp
    input_commands.cpp
    camera_rig_commands.cpp
    profile_commands.cpp
    image_encode.cpp
    stb_image_write_impl.cpp)
target_link_libraries(engine_remote_world PUBLIC
    engine_flags engine_core engine_input engine_remote engine_scene engine_physics engine_nav engine_weather
    engine_script engine_project)
target_include_directories(engine_remote_world PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_remote_world SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# stb's implementation TU is not warning-clean under our strict flags.
set_source_files_properties(stb_image_write_impl.cpp
    PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
