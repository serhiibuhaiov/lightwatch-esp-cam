// Host tests of the trigger "cron" expressions (lib/cron): pio test -e native
#include <unity.h>

#include "cron.h"

using Cron::Civil;
using Cron::Expr;

namespace {

Expr expr(const char* text) {
  Expr e;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(Cron::parse(text, e, why), (std::string(text) + ": " + why).c_str());
  return e;
}

bool bad(const char* text) {
  Expr e;
  std::string why;
  const bool ok = Cron::parse(text, e, why);
  return !ok && !why.empty();
}

std::string after(const char* text, Civil from) {
  Civil out;
  if (!Cron::next(expr(text), from, out)) return "none";
  return Cron::text(out);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_weekdays_of_known_dates() {
  TEST_ASSERT_EQUAL(4, Cron::weekday(1970, 1, 1));   // Thursday
  TEST_ASSERT_EQUAL(3, Cron::weekday(2026, 10, 21)); // Wednesday
  TEST_ASSERT_EQUAL(0, Cron::weekday(2026, 10, 25)); // Sunday: the clocks go back in Kyiv
  TEST_ASSERT_EQUAL(2, Cron::weekday(2028, 2, 29));  // a leap day, Tuesday
  TEST_ASSERT_EQUAL(6, Cron::weekday(2000, 1, 1));
}

void test_the_fields() {
  const Expr e = expr("*/15 6-22 1,15 JAN-mar mon-fri");
  TEST_ASSERT_TRUE(e.minutes == ((1ull << 0) | (1ull << 15) | (1ull << 30) | (1ull << 45)));
  TEST_ASSERT_EQUAL_HEX32(0x7FFFC0u, e.hours);
  TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 15), e.days);
  TEST_ASSERT_EQUAL_HEX16(0x0E, e.months);
  TEST_ASSERT_EQUAL_HEX8(0x3E, e.weekdays);
  TEST_ASSERT_FALSE(e.dayStar);
  TEST_ASSERT_FALSE(e.weekdayStar);
  TEST_ASSERT_EQUAL_HEX8(0x41, expr("0 0 * * 0,6").weekdays);
  TEST_ASSERT_EQUAL_HEX8(0x01, expr("0 0 * * 7").weekdays);  // 7 is Sunday
  TEST_ASSERT_TRUE(expr("5/20 * * * *").minutes == ((1ull << 5) | (1ull << 25) | (1ull << 45)));  // n/s: from n on
  TEST_ASSERT_TRUE(expr("10-20/5 * * * *").minutes == ((1ull << 10) | (1ull << 15) | (1ull << 20)));
  TEST_ASSERT_TRUE(expr("  0   21 * * *  ").dayStar);  // extra spaces are fine
}

void test_mistakes_are_rejected_with_a_reason() {
  const char* wrong[] = {"", "* * * *", "* * * * * *", "60 * * * *", "* 24 * * *", "* * 0 * *", "* * 32 * *", "* * * 13 *",
                         "* * * * 8", "5-1 * * * *", "*/0 * * * *", "1,,2 * * * *", "1, * * * *", "a * * * *", "* * * JANUARY *",
                         "@daily", "0 21 * * MON-", "*/ * * * *"};
  for (const char* w : wrong) TEST_ASSERT_TRUE_MESSAGE(bad(w), w);
  TEST_ASSERT_TRUE(bad("0 0 * * * 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"));
}

void test_matching_and_the_day_rule() {
  TEST_ASSERT_TRUE(Cron::matches(expr("0 21 * * *"), Civil{2026, 10, 21, 21, 0}));
  TEST_ASSERT_FALSE(Cron::matches(expr("0 21 * * *"), Civil{2026, 10, 21, 21, 1}));
  // Both day fields restricted: either one (the 1st, or any Sunday).
  const Expr either = expr("0 12 1 * 0");
  TEST_ASSERT_TRUE(Cron::matches(either, Civil{2026, 10, 1, 12, 0}));   // a Thursday, the 1st
  TEST_ASSERT_TRUE(Cron::matches(either, Civil{2026, 10, 25, 12, 0}));  // a Sunday
  TEST_ASSERT_FALSE(Cron::matches(either, Civil{2026, 10, 21, 12, 0}));
  // One of them *: both must hold (the * one always does).
  TEST_ASSERT_TRUE(Cron::matches(expr("0 12 * * 0"), Civil{2026, 10, 25, 12, 0}));
  TEST_ASSERT_FALSE(Cron::matches(expr("0 12 * * 0"), Civil{2026, 10, 1, 12, 0}));
  TEST_ASSERT_FALSE(Cron::matches(expr("0 12 */2 * 0"), Civil{2026, 10, 18, 12, 0}));  // */2 counts as * for the rule: both hold, 18 is even
}

void test_the_next_minute() {
  TEST_ASSERT_EQUAL_STRING("2026-10-21T21:00", after("0 21 * * *", Civil{2026, 10, 21, 14, 5}).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-21T21:00", after("0 21 * * *", Civil{2026, 10, 21, 21, 0}).c_str());  // at or after
  TEST_ASSERT_EQUAL_STRING("2026-10-22T21:00", after("0 21 * * *", Civil{2026, 10, 21, 21, 1}).c_str());
  TEST_ASSERT_EQUAL_STRING("2027-01-01T00:00", after("0 0 1 1 *", Civil{2026, 12, 31, 23, 59}).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-26T06:00", after("0 6 * * 1-5", Civil{2026, 10, 24, 7, 0}).c_str());  // Saturday -> Monday
  TEST_ASSERT_EQUAL_STRING("2028-02-29T00:00", after("0 0 29 2 *", Civil{2027, 3, 1, 0, 0}).c_str());
  TEST_ASSERT_EQUAL_STRING("none", after("0 0 30 2 *", Civil{2026, 1, 1, 0, 0}).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-21T14:15", after("*/15 * * * *", Civil{2026, 10, 21, 14, 1}).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_weekdays_of_known_dates);
  RUN_TEST(test_the_fields);
  RUN_TEST(test_mistakes_are_rejected_with_a_reason);
  RUN_TEST(test_matching_and_the_day_rule);
  RUN_TEST(test_the_next_minute);
  return UNITY_END();
}
