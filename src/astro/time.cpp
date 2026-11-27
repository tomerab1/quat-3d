// Astronomical time — see include/engine/astro/time.hpp.

#include "engine/astro/time.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace engine::astro {

namespace {

constexpr std::int64_t k_days_1970_to_2000 = 10957;
constexpr std::int64_t k_ns_per_minute = 60 * k_ns_per_second;
constexpr std::int64_t k_ns_per_hour = 60 * k_ns_per_minute;

// Floor division / modulo (C++ `/` truncates toward zero).
[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// A tiny cursor over the text being parsed.
class Cursor {
public:
    explicit Cursor(std::string_view text) : s_(text) {}
    [[nodiscard]] bool done() const { return i_ >= s_.size(); }
    [[nodiscard]] char peek() const { return done() ? '\0' : s_[i_]; }
    void skip() { ++i_; }
    [[nodiscard]] std::size_t pos() const { return i_; }
    // Exactly `n` decimal digits.
    [[nodiscard]] bool digits(int n, int& out) {
        int v = 0;
        for (int k = 0; k < n; ++k) {
            if (done() || std::isdigit(static_cast<unsigned char>(s_[i_])) == 0) return false;
            v = v * 10 + (s_[i_] - '0');
            ++i_;
        }
        out = v;
        return true;
    }
    [[nodiscard]] bool accept(char c) {
        if (peek() != c) return false;
        ++i_;
        return true;
    }

private:
    std::string_view s_;
    std::size_t      i_ = 0;
};

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
    return s;
}

// hh:mm[:ss[.fff…]] -> fields; an error names the bad part.
[[nodiscard]] std::expected<void, std::string> parse_clock(Cursor& c, CivilTime& out) {
    if (!c.digits(2, out.hour)) return std::unexpected("expected the hour as two digits (hh)");
    if (!c.accept(':')) return std::unexpected("expected ':' after the hour (hh:mm)");
    if (!c.digits(2, out.minute)) return std::unexpected("expected the minute as two digits (mm)");
    out.second = 0.0;
    if (c.accept(':')) {
        int whole = 0;
        if (!c.digits(2, whole)) return std::unexpected("expected the second as two digits (ss)");
        double frac = 0.0;
        if (c.accept('.') || c.accept(',')) {
            double scale = 0.1;
            int    n = 0;
            while (std::isdigit(static_cast<unsigned char>(c.peek())) != 0) {
                if (n < 9) frac += scale * (c.peek() - '0');
                scale *= 0.1;
                ++n;
                c.skip();
            }
            if (n == 0) return std::unexpected("expected digits after the decimal point of the second");
        }
        out.second = whole + frac;
    }
    if (out.hour > 23) return std::unexpected("hour " + std::to_string(out.hour) + " out of range 0..23");
    if (out.minute > 59) {
        return std::unexpected("minute " + std::to_string(out.minute) + " out of range 0..59");
    }
    if (out.second >= 60.0) return std::unexpected("second out of range 0..59 (leap seconds are not represented)");
    return {};
}

} // namespace

std::int64_t days_from_civil(int year, int month, int day) {
    const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t m = month;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(std::int64_t days, int& year, int& month, int& day) {
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

int days_in_month(int year, int month) {
    static constexpr std::array<int, 12> k_days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return k_days[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0);
}

bool valid_civil(const CivilTime& c) {
    return c.year >= k_min_year && c.year <= k_max_year && c.month >= 1 && c.month <= 12 &&
           c.day >= 1 && c.day <= days_in_month(c.year, c.month) && c.hour >= 0 && c.hour <= 23 &&
           c.minute >= 0 && c.minute <= 59 && std::isfinite(c.second) && c.second >= 0.0 &&
           c.second < 60.0;
}

UtcTime utc_from_civil(const CivilTime& c, int utc_offset_min) {
    const std::int64_t days = days_from_civil(c.year, c.month, c.day) - k_days_1970_to_2000;
    const std::int64_t ns = days * k_ns_per_day + c.hour * k_ns_per_hour +
                            c.minute * k_ns_per_minute + std::llround(c.second * 1e9) -
                            static_cast<std::int64_t>(utc_offset_min) * k_ns_per_minute;
    return UtcTime{ns};
}

CivilTime civil_from_utc(UtcTime t, int utc_offset_min) {
    const std::int64_t local = t.ns + static_cast<std::int64_t>(utc_offset_min) * k_ns_per_minute;
    const std::int64_t days = floor_div(local, k_ns_per_day);
    std::int64_t       rem = local - days * k_ns_per_day;
    CivilTime          c;
    civil_from_days(days + k_days_1970_to_2000, c.year, c.month, c.day);
    c.hour = static_cast<int>(rem / k_ns_per_hour);
    rem -= c.hour * k_ns_per_hour;
    c.minute = static_cast<int>(rem / k_ns_per_minute);
    rem -= c.minute * k_ns_per_minute;
    c.second = static_cast<double>(rem) * 1e-9;
    return c;
}

bool in_supported_range(UtcTime t) {
    const int y = civil_from_utc(t).year;
    return y >= k_min_year && y <= k_max_year;
}

std::expected<ParsedDateTime, std::string> parse_iso8601(std::string_view text) {
    const std::string_view s = trim(text);
    Cursor                 c(s);
    ParsedDateTime         out;
    CivilTime&             v = out.civil;
    if (!c.digits(4, v.year) || !c.accept('-') || !c.digits(2, v.month) || !c.accept('-') ||
        !c.digits(2, v.day)) {
        return std::unexpected("expected an ISO-8601 date-time YYYY-MM-DDThh:mm[:ss][Z|+hh:mm], got \"" +
                               std::string(s.substr(0, 40)) + "\"");
    }
    if (v.month < 1 || v.month > 12) {
        return std::unexpected("month " + std::to_string(v.month) + " out of range 1..12");
    }
    if (v.day < 1 || v.day > days_in_month(v.year, v.month)) {
        return std::unexpected("day " + std::to_string(v.day) + " out of range 1.." +
                               std::to_string(days_in_month(v.year, v.month)) + " for " +
                               std::to_string(v.year) + "-" + (v.month < 10 ? "0" : "") +
                               std::to_string(v.month));
    }
    if (v.year < k_min_year || v.year > k_max_year) {
        return std::unexpected("year " + std::to_string(v.year) + " outside the supported range " +
                               std::to_string(k_min_year) + ".." + std::to_string(k_max_year));
    }
    if (c.accept('T') || c.accept('t') || c.accept(' ')) {
        if (auto ok = parse_clock(c, v); !ok) return std::unexpected(ok.error());
        if (c.accept('Z') || c.accept('z')) {
            out.utc_offset_min = 0;
        } else if (c.peek() == '+' || c.peek() == '-') {
            const int sign = c.peek() == '-' ? -1 : 1;
            c.skip();
            int oh = 0;
            int om = 0;
            if (!c.digits(2, oh)) return std::unexpected("expected the UTC offset as ±hh:mm");
            if (c.accept(':')) {
                if (!c.digits(2, om)) return std::unexpected("expected the UTC offset as ±hh:mm");
            } else if (!c.done()) {
                if (!c.digits(2, om)) return std::unexpected("expected the UTC offset as ±hh:mm");
            }
            if (oh > 14 || om > 59 || oh * 60 + om > 14 * 60) {
                return std::unexpected("UTC offset out of range -14:00..+14:00");
            }
            out.utc_offset_min = sign * (oh * 60 + om);
        }
    }
    if (!c.done()) {
        return std::unexpected("unexpected text after the date-time at offset " +
                               std::to_string(c.pos()) + ": \"" + std::string(s.substr(c.pos(), 20)) +
                               "\"");
    }
    return out;
}

std::expected<double, std::string> parse_time_of_day(std::string_view text) {
    const std::string_view s = trim(text);
    Cursor                 c(s);
    CivilTime              v;
    if (auto ok = parse_clock(c, v); !ok) return std::unexpected(ok.error() + " (expected hh:mm[:ss])");
    if (!c.done()) return std::unexpected("unexpected text after the time of day (expected hh:mm[:ss])");
    return v.hour * 3600.0 + v.minute * 60.0 + v.second;
}

std::string format_iso8601(UtcTime t, int utc_offset_min, int fraction_digits) {
    const std::int64_t local = t.ns + static_cast<std::int64_t>(utc_offset_min) * k_ns_per_minute;
    const std::int64_t days = floor_div(local, k_ns_per_day);
    std::int64_t       rem = local - days * k_ns_per_day;
    int                y = 0;
    int                mo = 0;
    int                d = 0;
    civil_from_days(days + k_days_1970_to_2000, y, mo, d);
    const auto h = static_cast<int>(rem / k_ns_per_hour);
    rem -= h * k_ns_per_hour;
    const auto mi = static_cast<int>(rem / k_ns_per_minute);
    rem -= mi * k_ns_per_minute;
    const auto sec = static_cast<int>(rem / k_ns_per_second);
    const std::int64_t frac_ns = rem - sec * k_ns_per_second;

    std::array<char, 96> buf{};
    int n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d", y, mo, d, h, mi, sec);
    std::string out(buf.data(), static_cast<std::size_t>(n));
    if (fraction_digits > 0) {
        const int digits = fraction_digits > 9 ? 9 : fraction_digits;
        std::int64_t f = frac_ns;
        for (int k = digits; k < 9; ++k) f /= 10;
        n = std::snprintf(buf.data(), buf.size(), ".%0*lld", digits, static_cast<long long>(f));
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
    if (utc_offset_min == 0) {
        out += 'Z';
    } else {
        const int a = utc_offset_min < 0 ? -utc_offset_min : utc_offset_min;
        n = std::snprintf(buf.data(), buf.size(), "%c%02d:%02d", utc_offset_min < 0 ? '-' : '+', a / 60,
                          a % 60);
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
    return out;
}

double days_ut_since_j2000(UtcTime t) {
    // Split into whole days and the remainder so the double keeps full
    // sub-millisecond precision far from the epoch.
    const std::int64_t shifted = t.ns - k_ns_per_day / 2; // J2000.0 is 12:00
    const std::int64_t days = floor_div(shifted, k_ns_per_day);
    const std::int64_t rem = shifted - days * k_ns_per_day;
    return static_cast<double>(days) + static_cast<double>(rem) / static_cast<double>(k_ns_per_day);
}

double julian_date_ut(UtcTime t) { return k_jd_j2000 + days_ut_since_j2000(t); }

double delta_t_seconds(double y) {
    // Espenak & Meeus (2006), "Polynomial expressions for Delta T", valid
    // 1800..2150 here; outside it the long-term parabola.
    if (y < 1800.0 || y >= 2150.0) {
        const double u = (y - 1820.0) / 100.0;
        return -20.0 + 32.0 * u * u;
    }
    if (y < 1860.0) {
        const double t = y - 1800.0;
        return 13.72 - 0.332447 * t + 0.0068612 * t * t + 0.0041116 * t * t * t -
               0.00037436 * std::pow(t, 4) + 0.0000121272 * std::pow(t, 5) -
               0.0000001699 * std::pow(t, 6) + 0.000000000875 * std::pow(t, 7);
    }
    if (y < 1900.0) {
        const double t = y - 1860.0;
        return 7.62 + 0.5737 * t - 0.251754 * t * t + 0.01680668 * t * t * t -
               0.0004473624 * std::pow(t, 4) + std::pow(t, 5) / 233174.0;
    }
    if (y < 1920.0) {
        const double t = y - 1900.0;
        return -2.79 + 1.494119 * t - 0.0598939 * t * t + 0.0061966 * t * t * t -
               0.000197 * std::pow(t, 4);
    }
    if (y < 1941.0) {
        const double t = y - 1920.0;
        return 21.20 + 0.84493 * t - 0.076100 * t * t + 0.0020936 * t * t * t;
    }
    if (y < 1961.0) {
        const double t = y - 1950.0;
        return 29.07 + 0.407 * t - t * t / 233.0 + t * t * t / 2547.0;
    }
    if (y < 1986.0) {
        const double t = y - 1975.0;
        return 45.45 + 1.067 * t - t * t / 260.0 - t * t * t / 718.0;
    }
    if (y < 2005.0) {
        const double t = y - 2000.0;
        return 63.86 + 0.3345 * t - 0.060374 * t * t + 0.0017275 * t * t * t +
               0.000651814 * std::pow(t, 4) + 0.00002373599 * std::pow(t, 5);
    }
    if (y < 2050.0) {
        const double t = y - 2000.0;
        return 62.92 + 0.32217 * t + 0.005589 * t * t;
    }
    const double u = (y - 1820.0) / 100.0;
    return -20.0 + 32.0 * u * u - 0.5628 * (2150.0 - y);
}

double days_tt_since_j2000(UtcTime t) {
    const double d_ut = days_ut_since_j2000(t);
    const double year = 2000.0 + (d_ut + 0.5) / 365.2425;
    return d_ut + delta_t_seconds(year) / 86400.0;
}

UtcTime add_seconds(UtcTime t, double seconds) {
    constexpr double k_limit = 9.0e9; // seconds; |ns| stays inside int64
    if (!std::isfinite(seconds)) return t;
    const double clamped = seconds > k_limit ? k_limit : (seconds < -k_limit ? -k_limit : seconds);
    const std::int64_t delta = std::llround(clamped * 1e9);
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    if (delta > 0 && t.ns > k_max - delta) return UtcTime{k_max};
    if (delta < 0 && t.ns < k_min - delta) return UtcTime{k_min};
    return UtcTime{t.ns + delta};
}

} // namespace engine::astro
