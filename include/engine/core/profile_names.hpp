#pragma once

// Zone names of the built-in frame profiler (ROADMAP R4.2), interned into
// small integer ids so the per-scope record stays 24 bytes and statistics are
// indexed arrays. The table is process-wide and thread-safe; call sites
// intern once and cache the id (the QUAT_ZONE macros keep it in a static).
// Also names threads for the profiler's records.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

// What a zone measures. `wait` zones block on the GPU or the presentation
// engine (fence waits, image acquire, present): the frame's wait time is their
// sum, and its CPU time is the wall time minus that.
enum class ProfileCategory : std::uint8_t { scope, system, pass, wait };

[[nodiscard]] std::string_view to_string(ProfileCategory category);

using ProfileNameId = std::uint16_t;

// Distinct names the table holds. Interning past it returns
// k_profile_overflow_name ("(other)"), so the profiler never allocates for a
// runaway set of dynamic names.
inline constexpr std::uint32_t k_max_profile_names = 1024;
inline constexpr ProfileNameId k_profile_overflow_name = 0;
// Longest name kept; longer names are truncated before interning.
inline constexpr std::size_t k_max_profile_name_length = 96;

// The id of `name`, interning it with `category` the first time (a later
// call with another category keeps the first one).
[[nodiscard]] ProfileNameId intern_profile_name(std::string_view name,
                                                ProfileCategory category = ProfileCategory::scope);
// Name / category of an interned id ("(other)" / scope for unknown ids).
[[nodiscard]] std::string_view profile_name(ProfileNameId id);
[[nodiscard]] ProfileCategory profile_category(ProfileNameId id);
// Ids handed out so far (overflow entry included).
[[nodiscard]] std::uint32_t profile_name_count();

// Index of threads that never named themselves (and of every thread past the
// 254th).
inline constexpr std::uint8_t k_unnamed_profile_thread = 0xFF;

// Names the calling thread in profiler records and returns its index (indices
// count up in naming order). Naming a thread again keeps its index and
// replaces the name.
std::uint8_t set_profile_thread_name(std::string_view name);
// The calling thread's index (k_unnamed_profile_thread until it is named).
[[nodiscard]] std::uint8_t profile_thread_index();
// Names by thread index.
[[nodiscard]] std::vector<std::string> profile_thread_names();

} // namespace engine::core
