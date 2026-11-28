#pragma once

// Profiling facade (ROADMAP R4.1). Every instrumentation point goes through
// these macros; they feed the built-in frame profiler (always on, see
// frame_profiler.hpp) and, in ENGINE_PROFILE builds (QUAT_TRACY defined),
// Tracy — on demand, so a build without a connected viewer pays only a
// connection check per zone.
//
//   QUAT_ZONE()                   zone named after the enclosing function
//   QUAT_ZONE_N("name")           zone with a literal name
//   QUAT_ZONE_C("name", category) literal name with a ProfileCategory
//                                 (system, pass, wait; scope by default)
//   QUAT_ZONE_DYNAMIC(id, name)   runtime name: `id` from intern_profile_name,
//                                 `name` a NUL-terminated string
//   QUAT_FRAME_MARK()             end of frame (built-in end_frame + Tracy)
//   QUAT_PLOT("name", value)      per-frame counter / Tracy plot
//   QUAT_THREAD_NAME("name")      name the calling thread
//
// Include this header only from .cpp files: it pulls in Tracy's header when
// QUAT_TRACY is on, and no engine header may expose Tracy.

#include "engine/core/frame_profiler.hpp"
#include "engine/core/profile_names.hpp"

#if defined(QUAT_TRACY)
#include <tracy/Tracy.hpp>
#endif

#define QUAT_PROFILE_CONCAT_INNER(a, b) a##b
#define QUAT_PROFILE_CONCAT(a, b) QUAT_PROFILE_CONCAT_INNER(a, b)
#define QUAT_PROFILE_VAR(prefix) QUAT_PROFILE_CONCAT(prefix, __LINE__)

#if defined(QUAT_TRACY)
// Line-unique variable names (Tracy's ZoneScoped uses one fixed name, which
// -Wshadow flags for nested zones).
#define QUAT_TRACY_ZONE_FUNCTION() ZoneNamed(QUAT_PROFILE_VAR(quat_tracy_zone_), true)
#define QUAT_TRACY_ZONE_NAMED(name) ZoneNamedN(QUAT_PROFILE_VAR(quat_tracy_zone_), name, true)
#define QUAT_TRACY_ZONE_TRANSIENT(name) ZoneTransientN(QUAT_PROFILE_VAR(quat_tracy_zone_), name, true)
#define QUAT_TRACY_FRAME_MARK() FrameMark
#define QUAT_TRACY_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#define QUAT_TRACY_THREAD_NAME(name) ::tracy::SetThreadName(name)
#else
#define QUAT_TRACY_ZONE_FUNCTION() static_cast<void>(0)
#define QUAT_TRACY_ZONE_NAMED(name) static_cast<void>(0)
#define QUAT_TRACY_ZONE_TRANSIENT(name) static_cast<void>(0)
#define QUAT_TRACY_FRAME_MARK() static_cast<void>(0)
#define QUAT_TRACY_PLOT(name, value) static_cast<void>(0)
#define QUAT_TRACY_THREAD_NAME(name) static_cast<void>(0)
#endif

// Built-in scope under a name interned once per call site.
#define QUAT_BUILTIN_ZONE(name, category)                                                        \
    static const ::engine::core::ProfileNameId QUAT_PROFILE_VAR(quat_profile_id_) =              \
        ::engine::core::intern_profile_name(name, category);                                     \
    const ::engine::core::ProfileScope QUAT_PROFILE_VAR(quat_profile_scope_) {                   \
        QUAT_PROFILE_VAR(quat_profile_id_)                                                       \
    }

#define QUAT_ZONE()                                                                              \
    QUAT_BUILTIN_ZONE(__func__, ::engine::core::ProfileCategory::scope);                         \
    QUAT_TRACY_ZONE_FUNCTION()

#define QUAT_ZONE_C(name, category)                                                              \
    QUAT_BUILTIN_ZONE(name, category);                                                           \
    QUAT_TRACY_ZONE_NAMED(name)

#define QUAT_ZONE_N(name) QUAT_ZONE_C(name, ::engine::core::ProfileCategory::scope)

#define QUAT_ZONE_DYNAMIC(id, name)                                                              \
    const ::engine::core::ProfileScope QUAT_PROFILE_VAR(quat_profile_scope_){id};               \
    QUAT_TRACY_ZONE_TRANSIENT(name)

#define QUAT_FRAME_MARK()                                                                        \
    do {                                                                                         \
        ::engine::core::profiler().end_frame();                                                  \
        QUAT_TRACY_FRAME_MARK();                                                                 \
    } while (false)

#define QUAT_PLOT(name, value)                                                                   \
    do {                                                                                         \
        static const ::engine::core::ProfileNameId quat_plot_id =                                \
            ::engine::core::intern_profile_name(name);                                           \
        ::engine::core::profiler().set_counter(quat_plot_id, static_cast<double>(value));        \
        QUAT_TRACY_PLOT(name, value);                                                            \
    } while (false)

#define QUAT_THREAD_NAME(name)                                                                   \
    do {                                                                                         \
        ::engine::core::set_profile_thread_name(name);                                           \
        QUAT_TRACY_THREAD_NAME(name);                                                            \
    } while (false)
