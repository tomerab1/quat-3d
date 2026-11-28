# ---------------------------------------------------------------------------
# engine_game — native game modules (ROADMAP G17.1): the Registrar games
# register through, field-table components, system phases and their
# scheduling, module remote methods with JSON Schemas (validated here; the
# application installs them on its endpoint), Lua modules and input action
# maps. Graphics-free and remote-free, so a shipping player links it too.
# ---------------------------------------------------------------------------
add_library(engine_game STATIC
    registrar.cpp
    module_set.cpp
    json_schema.cpp)
target_link_libraries(engine_game PUBLIC
    engine_flags engine_core engine_input engine_scene nlohmann_json::nlohmann_json EnTT::EnTT)
target_include_directories(engine_game PUBLIC ${ENGINE_PUBLIC_INCLUDE})
