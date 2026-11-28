# ---------------------------------------------------------------------------
# engine_input — action maps (G5): device state, JSON action maps, the
# Actions evaluator (contexts, triggers, modifiers, injection, rebinding) and
# the per-user rebinding file. Window-free: the app samples SDL3 into a
# DeviceState (src/app/input_sampler.cpp).
# ---------------------------------------------------------------------------
add_library(engine_input STATIC
    device_names.cpp
    binding_json.cpp
    action_map_json.cpp
    default_map.cpp
    source_read.cpp
    actions.cpp
    overrides.cpp)
target_link_libraries(engine_input PUBLIC engine_flags engine_core nlohmann_json::nlohmann_json)
target_include_directories(engine_input PUBLIC ${ENGINE_PUBLIC_INCLUDE})
