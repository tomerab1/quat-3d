#pragma once

// Astronomical time (ROADMAP R22.1): a UTC instant, the proleptic Gregorian
// calendar, ISO-8601 text, Julian dates and ΔT = TT − UT.
//
// An instant is an integer count of nanoseconds since 2000-01-01T00:00:00Z on
// a uniform scale (POSIX-style: leap seconds are not counted, so UTC is
// treated as UT1 to within < 1 s, which the ephemerides here cannot resolve
// anyway). Integer time makes the world clock's accumulation exact and
// deterministic; ±292 years fit, and the supported range is k_min_year ..
// k_max_year (the low-precision ephemerides and the ΔT fit are meant for it).
//
// Ephemerides take "days since J2000.0": d_ut on the UT scale (Earth
// rotation: sidereal time) and d_tt = d_ut + ΔT / 86400 on the TT scale
// (orbital motion). J2000.0 is JD 2451545.0 = 2000-01-01T12:00:00.
//
// References: Meeus, "Astronomical Algorithms", 2nd ed. (1998), ch. 7
// (Julian day); Hinnant, "chrono-Compatible Low-Level Date Algorithms"
// (days_from_civil / civil_from_days); Espenak & Meeus, "Five Millennium
// Canon of Solar Eclipses" (NASA/TP-2006-214141), ΔT polynomials.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace engine::astro {

inline constexpr int k_min_year = 1800;
inline constexpr int k_max_year = 2200;

inline constexpr std::int64_t k_ns_per_second = 1'000'000'000;
inline constexpr std::int64_t k_ns_per_day = 86'400 * k_ns_per_second;
inline constexpr double       k_jd_j2000 = 2451545.0; // 2000-01-01T12:00 (TT for ephemerides)

struct UtcTime {
    std::int64_t ns = 0; // since 2000-01-01T00:00:00Z

    friend constexpr bool operator==(UtcTime, UtcTime) = default;
    friend constexpr auto operator<=>(UtcTime, UtcTime) = default;
};

// A calendar date and time of day (proleptic Gregorian).
struct CivilTime {
    int    year = 2000;
    int    month = 1;  // 1..12
    int    day = 1;    // 1..days in month
    int    hour = 0;   // 0..23
    int    minute = 0; // 0..59
    double second = 0.0; // [0, 60)
};

// Days since 1970-01-01 of a proleptic Gregorian date (Hinnant).
[[nodiscard]] std::int64_t days_from_civil(int year, int month, int day);
// The proleptic Gregorian date `days` after 1970-01-01.
void civil_from_days(std::int64_t days, int& year, int& month, int& day);
[[nodiscard]] int days_in_month(int year, int month);

// The instant whose local time (UTC + `utc_offset_min`) is `civil`. The
// fields must be valid (see valid_civil()).
[[nodiscard]] UtcTime utc_from_civil(const CivilTime& civil, int utc_offset_min = 0);
// Local time (UTC + `utc_offset_min`) of `t`.
[[nodiscard]] CivilTime civil_from_utc(UtcTime t, int utc_offset_min = 0);
[[nodiscard]] bool valid_civil(const CivilTime& civil);
// True when the instant's UTC year is within [k_min_year, k_max_year].
[[nodiscard]] bool in_supported_range(UtcTime t);

// Parsed ISO-8601 date-time: YYYY-MM-DD[(T| )hh:mm[:ss[.fff…]]][Z|±hh[:mm]].
// `utc_offset_min` is empty when the text has no zone designator.
struct ParsedDateTime {
    CivilTime          civil;
    std::optional<int> utc_offset_min;
};
// A precise message on failure ("expected YYYY-MM-DD…", "month 13 out of
// range", …). Years outside [k_min_year, k_max_year] are rejected.
[[nodiscard]] std::expected<ParsedDateTime, std::string> parse_iso8601(std::string_view text);
// A local time of day "hh:mm[:ss[.fff…]]" as seconds after midnight.
[[nodiscard]] std::expected<double, std::string> parse_time_of_day(std::string_view text);

// "YYYY-MM-DDThh:mm:ss[.fff]" plus "Z" (offset 0) or "±hh:mm".
// `fraction_digits` (0..9) digits of the second are written (truncated).
[[nodiscard]] std::string format_iso8601(UtcTime t, int utc_offset_min = 0,
                                         int fraction_digits = 0);

// Days since J2000.0 on the UT scale (Earth rotation).
[[nodiscard]] double days_ut_since_j2000(UtcTime t);
// Julian date (UT) of `t`.
[[nodiscard]] double julian_date_ut(UtcTime t);
// ΔT = TT − UT in seconds for a decimal year (Espenak & Meeus 2006).
[[nodiscard]] double delta_t_seconds(double decimal_year);
// Days since J2000.0 on the TT scale (ephemerides): d_ut + ΔT / 86400.
[[nodiscard]] double days_tt_since_j2000(UtcTime t);
// Julian centuries of TT since J2000.0, the argument of Meeus' series.
[[nodiscard]] inline double centuries_tt(double days_tt) { return days_tt / 36525.0; }

// `t` moved by `seconds` (rounded to the nanosecond; saturates at the int64
// range, which lies far outside the supported years).
[[nodiscard]] UtcTime add_seconds(UtcTime t, double seconds);

} // namespace engine::astro
