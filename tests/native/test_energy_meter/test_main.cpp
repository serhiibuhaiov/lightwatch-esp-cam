// Host tests of the energy since boot (lib/energy_meter): pio test -e native
#include <unity.h>

#include <cmath>

#include "energy_meter.h"

using EnergyMeter::kNone;
using EnergyMeter::Meter;

namespace {
constexpr uint64_t kHour = 3600000;
}

// Unity has no doubles here: Wh compared as whole mWh, to within `mwh`.
#define WH_WITHIN(mwh, expected, actual) \
  TEST_ASSERT_INT64_WITHIN(mwh, std::llround((expected) * 1000.0), std::llround((actual) * 1000.0))
#define WH(expected, actual) WH_WITHIN(0, expected, actual)

void setUp() {}
void tearDown() {}

void test_nothing_before_the_first_state() {
  Meter m;
  TEST_ASSERT_FALSE(m.started());
  WH(0.0, m.integratedWh(kHour));
  TEST_ASSERT_EQUAL_UINT32(0, m.sinceS(kHour));
  TEST_ASSERT_EQUAL_UINT32(0, m.coveredS(kHour));
}

void test_a_steady_power_is_held_up_to_now() {
  Meter m;
  m.observe(1000, 1, 10000);  // 1000.0 W
  TEST_ASSERT_TRUE(m.holding());
  TEST_ASSERT_TRUE(m.counting());
  TEST_ASSERT_EQUAL_INT32(10000, m.heldPowerRaw());
  WH(1000.0, m.integratedWh(1000 + kHour));  // no new state: held for the hour
  TEST_ASSERT_EQUAL_UINT32(3600, m.coveredS(1000 + kHour));
  TEST_ASSERT_EQUAL_UINT32(3600, m.sinceS(1000 + kHour));
}

void test_each_power_holds_until_the_next_state() {
  Meter m;
  m.observe(0, 1, 20000);          // 2000 W for half an hour
  m.observe(kHour / 2, 1, 5000);   // 500 W for half an hour
  m.observe(kHour, 1, 5000);
  WH(1250.0, m.integratedWh(kHour));
}

void test_a_switch_that_is_off_is_zero_whatever_the_power_says() {
  Meter m;
  m.observe(0, 1, 10000);
  m.observe(kHour, 0, 10000);  // switched off, the socket still reports its old power
  WH(1000.0, m.integratedWh(2 * kHour));
  TEST_ASSERT_EQUAL_INT32(0, m.heldPowerRaw());
  TEST_ASSERT_EQUAL_UINT32(7200, m.coveredS(2 * kHour));  // off is known: the time counts, with 0 W
}

void test_no_power_reading_holds_nothing() {
  Meter m;
  m.observe(0, 1, kNone);  // a socket without a power data point
  TEST_ASSERT_TRUE(m.started());
  TEST_ASSERT_FALSE(m.holding());
  TEST_ASSERT_FALSE(m.counting());
  WH(0.0, m.integratedWh(kHour));
  TEST_ASSERT_EQUAL_UINT32(0, m.coveredS(kHour));
  TEST_ASSERT_EQUAL_UINT32(3600, m.sinceS(kHour));
}

void test_a_lost_socket_is_not_counted_until_it_answers_again() {
  Meter m;
  m.observe(0, 1, 10000);
  m.observe(kHour, 1, 10000);  // 1000 Wh
  m.lose(kHour + 60000);       // counted offline a minute later: the hour after the last answer is not counted
  TEST_ASSERT_FALSE(m.holding());
  WH(1000.0, m.integratedWh(3 * kHour));
  m.observe(3 * kHour, 1, 10000);  // back
  WH(2000.0, m.integratedWh(4 * kHour));
  TEST_ASSERT_EQUAL_UINT32(7200, m.coveredS(4 * kHour));
  TEST_ASSERT_EQUAL_UINT32(14400, m.sinceS(4 * kHour));
}

void test_short_pushes_add_up_exactly() {
  Meter m;
  uint64_t t = 0;
  for (int i = 0; i < 1200; ++i) {  // a push every 3 s for an hour, 1090.7 W
    m.observe(t, 1, 10907);
    t += 3000;
  }
  WH_WITHIN(1, 1090.7, m.integratedWh(kHour));
}

void test_a_state_back_in_time_changes_nothing() {
  Meter m;
  m.observe(10000, 1, 10000);
  m.observe(9999, 1, 20000);  // two observers a millisecond apart, the later one first
  WH_WITHIN(1, 2000.0, m.integratedWh(10000 + kHour));  // the newer power holds from 10000 on
}

void test_pushed_reports_always_count() {
  Meter m;
  TEST_ASSERT_TRUE(m.report(0, 10, true));
  TEST_ASSERT_TRUE(m.report(30000, 10, true));  // the same value again is another report
  TEST_ASSERT_TRUE(m.report(60000, 1, true));
  TEST_ASSERT_EQUAL_UINT64(21, m.reportedWh());
  TEST_ASSERT_EQUAL_UINT32(3, m.reports());
  TEST_ASSERT_EQUAL_UINT32(3, m.reportsCounted());
}

void test_a_read_value_counts_only_when_it_changed_and_the_first_is_the_base() {
  Meter m;
  TEST_ASSERT_FALSE(m.report(0, 32, false));  // the base: from before the board started, maybe
  TEST_ASSERT_TRUE(m.reportedStarted());
  TEST_ASSERT_FALSE(m.report(15000, 32, false));  // the same report read again
  TEST_ASSERT_TRUE(m.report(30000, 45, false));
  TEST_ASSERT_FALSE(m.report(45000, 45, false));
  TEST_ASSERT_EQUAL_UINT64(45, m.reportedWh());
  TEST_ASSERT_EQUAL_UINT32(4, m.reports());
  TEST_ASSERT_EQUAL_UINT32(1, m.reportsCounted());
  TEST_ASSERT_EQUAL_INT32(45, m.lastReportRaw());
}

void test_a_pushed_report_read_again_by_a_query_counts_once() {
  Meter m;
  m.report(0, 4, true);
  TEST_ASSERT_FALSE(m.report(1000, 4, false));
  TEST_ASSERT_TRUE(m.report(2000, 10, true));
  TEST_ASSERT_FALSE(m.report(3000, 10, false));
  TEST_ASSERT_EQUAL_UINT64(14, m.reportedWh());
}

void test_after_an_edit_a_read_value_is_a_new_base() {
  Meter m;
  m.report(0, 5, false);
  m.report(1000, 7, false);  // counted
  m.rebase();
  TEST_ASSERT_FALSE(m.report(2000, 300, false));  // another data point now: only the base
  TEST_ASSERT_TRUE(m.report(3000, 2, false));
  TEST_ASSERT_EQUAL_UINT64(9, m.reportedWh());
}

void test_negative_values_are_not_reports() {
  Meter m;
  TEST_ASSERT_FALSE(m.report(0, -1, true));
  TEST_ASSERT_FALSE(m.reportedStarted());
  TEST_ASSERT_EQUAL_INT32(kNone, m.lastReportRaw());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_before_the_first_state);
  RUN_TEST(test_a_steady_power_is_held_up_to_now);
  RUN_TEST(test_each_power_holds_until_the_next_state);
  RUN_TEST(test_a_switch_that_is_off_is_zero_whatever_the_power_says);
  RUN_TEST(test_no_power_reading_holds_nothing);
  RUN_TEST(test_a_lost_socket_is_not_counted_until_it_answers_again);
  RUN_TEST(test_short_pushes_add_up_exactly);
  RUN_TEST(test_a_state_back_in_time_changes_nothing);
  RUN_TEST(test_pushed_reports_always_count);
  RUN_TEST(test_a_read_value_counts_only_when_it_changed_and_the_first_is_the_base);
  RUN_TEST(test_a_pushed_report_read_again_by_a_query_counts_once);
  RUN_TEST(test_after_an_edit_a_read_value_is_a_new_base);
  RUN_TEST(test_negative_values_are_not_reports);
  return UNITY_END();
}
