#pragma once

// The one way engine sources include sol2 (G7). The engine builds without
// exceptions, so sol2 runs in its no-exceptions mode with every safety check
// on (argument types are verified; failures become Lua errors raised inside
// a protected call). CMake defines the same macros for engine_script; they
// are repeated here so a translation unit compiled on its own (the
// compile-check script) matches.

#ifndef SOL_NO_EXCEPTIONS
#define SOL_NO_EXCEPTIONS 1
#endif
#ifndef SOL_ALL_SAFETIES_ON
#define SOL_ALL_SAFETIES_ON 1
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#endif
#include <sol/sol.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
