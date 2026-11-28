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
    surface_query.cpp
    image_encode.cpp
    stb_image_write_impl.cpp)
# Command modules (ROADMAP S9): one <domain>_commands.cpp per domain, each
# defining register_<file stem>(RpcServer&, const ContextPtr&). The call list is
# generated from the directory: a new module is a new file.
quat_generate_registration(engine_remote_world
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PATTERN   "*_command*.cpp"
    TEMPLATE  ${CMAKE_CURRENT_SOURCE_DIR}/command_modules.cpp.in
    OUTPUT    ${CMAKE_CURRENT_BINARY_DIR}/generated/command_modules.cpp
    DECLARE   "void register_@NAME@(RpcServer& server, const ContextPtr& ctx)"
    CALL      "    register_@NAME@(server, ctx)"
    INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(engine_remote_world PUBLIC
    engine_flags engine_core engine_input engine_remote engine_scene engine_physics engine_nav engine_weather
    engine_script engine_project)
target_include_directories(engine_remote_world PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_remote_world SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# stb's implementation TU is not warning-clean under our strict flags.
set_source_files_properties(stb_image_write_impl.cpp
    PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
