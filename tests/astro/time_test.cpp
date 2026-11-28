// engine/astro/time.hpp: calendar, ISO-8601, Julian dates, ΔT.

#include <gtest/gtest.h>

#include "astro/astro_test_util.hpp"
#include "engine/astro/time.hpp"

namespace {

namespace a = engine::astro;
using quat::test::utc;

TEST(AstroTime, JulianDatesMatchMeeusChapter7) {
    // Meeus, Astronomical Algorithms (2nd ed.), Example 7.a and the table
    // on p. 62.
    EXPECT_NEAR(a::julian_date_ut(utc(1957, 10, 4, 19, 26, 24.0)), 2436116.31, 1e-6);
    EXPECT_DOUBLE_EQ(a::julian_date_ut(utc(2000, 1, 1, 12)), 2451545.0);
    EXPECT_DOUBLE_EQ(a::julian_date_ut(utc(1999, 1, 1)), 2451179.5);
    EXPECT_DOUBLE_EQ(a::julian_date_ut(utc(1987, 6, 19, 12)), 2446966.0);
    EXPECT_DOUBLE_EQ(a::julian_date_ut(utc(1988, 1, 27)), 2447187.5);
    EXPECT_DOUBLE_EQ(a::julian_date_ut(utc(1900, 1, 1)), 2415020.5);
}

TEST(AstroTime, CivilRoundTripsAcrossTheSupportedRange) {
    for (int year : {1800, 1899, 1900, 1999, 2000, 2024, 2100, 2200}) {
        for (int month : {1, 2, 3, 12}) {
            const int        day = a::days_in_month(year, month);
            const a::CivilTime c{year, month, day, 23, 59, 59.25};
            const a::UtcTime   t = a::utc_from_civil(c, -330);
            const a::CivilTime back = a::civil_from_utc(t, -330);
            EXPECT_EQ(back.year, year);
            EXPECT_EQ(back.month, month);
            EXPECT_EQ(back.day, day);
            EXPECT_EQ(back.hour, 23);
            EXPECT_EQ(back.minute, 59);
            EXPECT_NEAR(back.second, 59.25, 1e-9);
        }
    }
    EXPECT_EQ(a::days_in_month(1900, 2), 28); // not a leap year
    EXPECT_EQ(a::days_in_month(2000, 2), 29);
    EXPECT_EQ(a::days_in_month(2024, 2), 29);
}

TEST(AstroTime, ParsesIso8601WithAndWithoutZones) {
    auto p = a::parse_iso8601("2026-06-21T05:12:30.5+02:00");
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(p->civil.year, 2026);
    EXPECT_EQ(p->civil.hour, 5);
    EXPECT_NEAR(p->civil.second, 30.5, 1e-12);
    ASSERT_TRUE(p->utc_offset_min.has_value());
    EXPECT_EQ(*p->utc_offset_min, 120);
    EXPECT_EQ(a::format_iso8601(a::utc_from_civil(p->civil, 120), 0, 1), "2026-06-21T03:12:30.5Z");

    p = a::parse_iso8601(" 1999-12-31 23:59Z ");
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(*p->utc_offset_min, 0);

    p = a::parse_iso8601("2026-01-02");
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_FALSE(p->utc_offset_min.has_value());
    EXPECT_EQ(p->civil.hour, 0);

    p = a::parse_iso8601("2026-03-01T10:00-0330");
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(*p->utc_offset_min, -210);

    EXPECT_EQ(a::format_iso8601(utc(2026, 3, 1, 13, 30), -210), "2026-03-01T10:00:00-03:30");
    EXPECT_EQ(a::format_iso8601(utc(2026, 3, 1, 13, 30), 330), "2026-03-01T19:00:00+05:30");
}

TEST(AstroTime, RejectsMalformedDatesWithPreciseMessages) {
    const auto message = [](std::string_view text) {
        auto r = a::parse_iso8601(text);
        return r ? std::string("<accepted>") : r.error();
    };
    EXPECT_NE(message("2026-13-01").find("month 13"), std::string::npos);
    EXPECT_NE(message("2026-02-30").find("day 30 out of range 1..28"), std::string::npos);
    EXPECT_NE(message("2026-06-21T24:00").find("hour 24"), std::string::npos);
    EXPECT_NE(message("2026-06-21T12:60").find("minute 60"), std::string::npos);
    EXPECT_NE(message("1700-01-01").find("supported range 1800..2200"), std::string::npos);
    EXPECT_NE(message("2026-06-21T12:00+15:00").find("UTC offset"), std::string::npos);
    EXPECT_NE(message("tomorrow").find("YYYY-MM-DD"), std::string::npos);
    EXPECT_NE(message("2026-06-21T12:00 junk").find("unexpected text"), std::string::npos);
    EXPECT_NE(message("2026-06-21T12").find("':'"), std::string::npos);

    EXPECT_NEAR(*a::parse_time_of_day("06:30"), 6.5 * 3600.0, 1e-9);
    EXPECT_NEAR(*a::parse_time_of_day("23:59:30.25"), 86370.25, 1e-9);
    EXPECT_FALSE(a::parse_time_of_day("6:30").has_value());
    EXPECT_FALSE(a::parse_time_of_day("25:00").has_value());
}

TEST(AstroTime, DeltaTFollowsTheObservedRecord) {
    // Espenak & Meeus (2006) table of observed ΔT: 1900 −2.7 s, 1950 +29.1 s,
    // 1980 +50.5 s, 2000 +63.8 s.
    EXPECT_NEAR(a::delta_t_seconds(1900.0), -2.7, 0.5);
    EXPECT_NEAR(a::delta_t_seconds(1950.0), 29.1, 0.5);
    EXPECT_NEAR(a::delta_t_seconds(1980.0), 50.5, 0.5);
    EXPECT_NEAR(a::delta_t_seconds(2000.0), 63.8, 0.5);
    // The polynomial pieces join within a second.
    for (double y : {1860.0, 1900.0, 1920.0, 1941.0, 1961.0, 1986.0, 2005.0, 2050.0, 2150.0}) {
        EXPECT_NEAR(a::delta_t_seconds(y - 1e-6), a::delta_t_seconds(y), 1.0) << y;
    }
    // TT runs ahead of UT by ΔT.
    const a::UtcTime t = utc(2000, 1, 1, 12);
    EXPECT_NEAR((a::days_tt_since_j2000(t) - a::days_ut_since_j2000(t)) * 86400.0, 63.8, 0.5);
}

TEST(AstroTime, AddSecondsIsExactAndSaturates) {
    a::UtcTime t = utc(2026, 1, 1);
    for (int i = 0; i < 3600; ++i) t = a::add_seconds(t, 1.0 / 60.0 * 60.0);
    EXPECT_EQ(t, utc(2026, 1, 1, 1));
    EXPECT_EQ(a::add_seconds(t, std::numeric_limits<double>::infinity()), t);
    EXPECT_TRUE(a::in_supported_range(utc(2200, 12, 31, 23, 59, 59.0)));
    EXPECT_FALSE(a::in_supported_range(a::add_seconds(utc(2200, 12, 31, 23, 59, 59.0), 2.0)));
}

} // namespace
