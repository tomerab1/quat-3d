# ---------------------------------------------------------------------------
# engine_astro — astronomy for the time of day (R22.1): calendar / Julian
# dates, sidereal time, precession, the Meeus Sun and Moon, rise / set,
# and the renderer-facing SkyState. Pure, deterministic double-precision CPU
# code; glm only.
# ---------------------------------------------------------------------------
add_library(engine_astro STATIC
    time.cpp
    coords.cpp
    sun.cpp
    moon.cpp
    moon_terms.cpp
    ephemeris.cpp
    events.cpp
    sky_state.cpp)
target_link_libraries(engine_astro PUBLIC engine_flags glm::glm)
target_include_directories(engine_astro PUBLIC ${ENGINE_PUBLIC_INCLUDE})
