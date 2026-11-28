# ---------------------------------------------------------------------------
# engine_audio — miniaudio mixer, buses and 3D sound (G1). miniaudio is an
# implementation detail (PRIVATE); the public header is miniaudio-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_audio STATIC
    audio_engine.cpp
    miniaudio_impl.cpp)
target_link_libraries(engine_audio PUBLIC engine_flags glm::glm)
# miniaudio_headers is a local imported target holding a build-machine path:
# BUILD_INTERFACE keeps it out of the SDK export; its link libraries (threads,
# libdl, libm) are repeated so the exported static library carries them.
target_link_libraries(engine_audio PRIVATE $<BUILD_INTERFACE:miniaudio_headers> engine_core
    Threads::Threads ${CMAKE_DL_LIBS} $<$<PLATFORM_ID:Linux,FreeBSD>:m>)
target_include_directories(engine_audio PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# miniaudio's implementation is not warning-clean under our strict flags.
set_source_files_properties(miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
