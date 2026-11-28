# ---------------------------------------------------------------------------
# engine_weather — the scene's weather (R13.1): the WeatherState singleton
# component, its field table and accumulation, and the CPU reference of the
# global wind field the renderer evaluates on the GPU. Graphics-free.
# ---------------------------------------------------------------------------
add_library(engine_weather STATIC
    weather_state.cpp
    wind_field.cpp)
target_link_libraries(engine_weather PUBLIC engine_flags glm::glm EnTT::EnTT)
target_include_directories(engine_weather PUBLIC ${ENGINE_PUBLIC_INCLUDE})
