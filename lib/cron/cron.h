#pragma once
//
// A cron expression for the trigger "cron": "minute hour day-of-month month day-of-week", local time (config time.tz).
//   each field: * | n | a-b | */s | a-b/s | n/s (n to the end, every s), and lists of them with commas
//   minute 0-59, hour 0-23, day of month 1-31, month 1-12 or JAN-DEC, day of week 0-7 or SUN-SAT (0 and 7 are Sunday)
//   when both day fields are restricted (neither starts with *), a day matching either one is a match (as in Vixie cron)
// Examples: "0 21 * * *" (21:00 every day), "*/15 6-22 * * 1-5", "30 5 1,15 * *".
// Plain C++ on the civil (local) calendar, no time zone here; host tests in tests/native/test_cron.
//
#include <cstdint>
#include <string>

namespace Cron {

constexpr size_t kMaxTextBytes = 100;

struct Expr {
  uint64_t minutes = 0;   // bit n: minute n
  uint32_t hours = 0;     // bit n: hour n
  uint32_t days = 0;      // bit n: day of month n (1..31)
  uint16_t months = 0;    // bit n: month n (1..12)
  uint8_t weekdays = 0;   // bit n: day of week n (0 = Sunday .. 6)
  bool dayStar = true, weekdayStar = true;  // the field was * (or */s): see the day rule above
};

struct Civil {  // a local date and time to the minute
  int year = 1970, month = 1, day = 1, hour = 0, minute = 0;
};

// False with `why` when the text is not a valid expression.
bool parse(const char* text, Expr& out, std::string& why);

int weekday(int year, int month, int day);  // 0 = Sunday
bool matches(const Expr& e, const Civil& t);

// The first minute at or after `from` that matches, looking at most `maxDays` days ahead (false: none in that time,
// e.g. "0 0 30 2 *").
bool next(const Expr& e, const Civil& from, Civil& out, int maxDays = 366);

// "2026-10-21T21:00"
std::string text(const Civil& t);

}  // namespace Cron
