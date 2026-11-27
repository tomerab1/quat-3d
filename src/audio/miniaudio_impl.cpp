// Single translation unit that emits the miniaudio implementation. Compiled
// permissively (COMPILE_OPTIONS "-w"): miniaudio is not warning-clean under
// the engine's strict flags. Everywhere else (audio_engine.cpp only) includes
// <miniaudio.h> for declarations. vcpkg's miniaudio port is header-only.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
