// Host tests of the power rule decision (lib/power_rule): pio test -e native
#include <unity.h>

#include <cstring>

#include "power_rule.h"

using PowerRule::Sample;

namespace {

constexpr size_t kCap = 80;
constexpr uint32_t kInterval = 15;

Sample on(uint32_t t, int32_t power) {
  Sample s;
  s.t = t;
  s.v[0] = power;
  s.flags = Sample::kOk | Sample::kRelayKnown | Sample::kRelayOn;
  return s;
}

Sample off(uint32_t t) {
  Sample s;
  s.t = t;
  s.v[0] = 0;
  s.flags = Sample::kOk | Sample::kRelayKnown;
  return s;
}

Sample failed(uint32_t t) {
  Sample s;
  s.t = t;
  return s;
}

// `n` answers with the relay on and `power`, one per poll interval, from `t0`.
void addOn(std::vector<Sample>& s, uint32_t t0, size_t n, int32_t power) {
  for (size_t i = 0; i < n; ++i) s.push_back(on(t0 + static_cast<uint32_t>(i) * kInterval, power));
}

PowerRule::Result below(const std::vector<Sample>& s, uint32_t watts, uint32_t forS, uint32_t warmupS, size_t cap = kCap) {
  return PowerRule::evaluate(s, cap, s.empty() ? 0 : s.back().t + 1, kInterval, true, watts, forS, warmupS);
}

void assertReason(const char* want, const PowerRule::Result& r) { TEST_ASSERT_EQUAL_STRING(want, r.reason); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_no_history_never_fires() {
  const PowerRule::Result r = below({}, 100, 10, 0);
  TEST_ASSERT_FALSE(r.met);
  assertReason("no-data", r);
}

void test_an_old_last_answer_is_stale() {
  std::vector<Sample> s;
  addOn(s, 100, 5, 500);
  const PowerRule::Result r = PowerRule::evaluate(s, kCap, s.back().t + 38, kInterval, true, 100, 10, 0);  // > 2.5 intervals
  TEST_ASSERT_FALSE(r.met);
  assertReason("stale", r);
}

void test_the_last_answer_decides_the_reason() {
  std::vector<Sample> s;
  addOn(s, 100, 3, 500);
  s.push_back(failed(145));
  assertReason("unanswered", below(s, 100, 10, 0));
  s.back() = off(145);
  assertReason("relay-off", below(s, 100, 10, 0));
  Sample unknown = on(145, 500);
  unknown.flags = Sample::kOk;
  s.back() = unknown;
  assertReason("relay-unknown", below(s, 100, 10, 0));
  s.back() = on(145, PowerRule::kNone);
  assertReason("no-power-dp", below(s, 100, 10, 0));
  s.back() = on(145, 1500);  // 150 W, the rule wants below 100 W
  const PowerRule::Result r = below(s, 100, 10, 0);
  assertReason("not-met", r);
  TEST_ASSERT_EQUAL_INT32(1500, r.lastPower);
}

void test_below_fires_after_for_s() {
  std::vector<Sample> s;
  for (uint32_t i = 0; i < kCap - 10; ++i) s.push_back(on(i * kInterval, 2000));  // 200 W for a long time
  s.push_back(on(s.back().t + kInterval, 500));                                  // then 50 W
  PowerRule::Result r = below(s, 100, 30, 60);
  TEST_ASSERT_FALSE(r.met);
  assertReason("waiting", r);
  TEST_ASSERT_EQUAL_UINT32(0, r.runS);
  s.push_back(on(s.back().t + kInterval, 500));
  s.push_back(on(s.back().t + kInterval, 500));
  r = below(s, 100, 30, 60);  // no warm-up: the relay was on before the run
  TEST_ASSERT_TRUE(r.met);
  assertReason("met", r);
  TEST_ASSERT_EQUAL_UINT32(30, r.runS);
}

void test_above_is_the_mirror() {
  std::vector<Sample> s;
  s.push_back(on(0, 100));
  addOn(s, 15, 5, 3000);
  const PowerRule::Result r = PowerRule::evaluate(s, 6, s.back().t, kInterval, false, 200, 60, 0);
  TEST_ASSERT_TRUE(r.met);
  TEST_ASSERT_EQUAL_UINT32(60, r.runS);
}

void test_a_clean_off_before_the_run_means_warm_up() {
  std::vector<Sample> s;
  s.push_back(off(0));
  addOn(s, 15, 4, 500);  // on at 15..60
  PowerRule::Result r = below(s, 100, 30, 60);
  TEST_ASSERT_FALSE(r.met);
  assertReason("warmup", r);
  addOn(s, 75, 5, 500);  // up to 135: 135 - (15 + 60) = 60 s after the warm-up
  r = below(s, 100, 30, 60);
  TEST_ASSERT_TRUE(r.met);
  TEST_ASSERT_EQUAL_UINT32(60, r.runS);
}

void test_a_run_from_the_start_of_the_history_gets_the_warm_up() {
  // After a boot (the power came back) the socket answers "on" from the first poll: what happened before is unknown,
  // so it counts as just switched on. Before the fix such a rule fired after for_s alone.
  std::vector<Sample> s;
  addOn(s, 20, 4, 300);  // 20..65: 45 s, more than for_s
  PowerRule::Result r = below(s, 100, 40, 15);
  TEST_ASSERT_FALSE(r.met);
  assertReason("waiting", r);
  TEST_ASSERT_EQUAL_UINT32(30, r.runS);  // 65 - (20 + 15)
  r = below(s, 100, 30, 30);
  TEST_ASSERT_FALSE(r.met);
  TEST_ASSERT_EQUAL_UINT32(15, r.runS);
  s.push_back(on(80, 300));
  TEST_ASSERT_TRUE(below(s, 100, 30, 30).met);
}

void test_a_full_ring_needs_no_warm_up() {
  std::vector<Sample> s;
  addOn(s, 1000, kCap, 300);  // the run is older than the history: the relay has been on for a long time
  const PowerRule::Result r = below(s, 100, 900, 3600);
  TEST_ASSERT_TRUE(r.met);
  TEST_ASSERT_EQUAL_UINT32((kCap - 1) * kInterval, r.runS);
}

void test_a_failed_answer_restarts_the_count() {
  std::vector<Sample> s;
  addOn(s, 0, 70, 2000);
  addOn(s, 70 * kInterval, 3, 300);
  s.push_back(failed(73 * kInterval));
  addOn(s, 74 * kInterval, 2, 300);
  const PowerRule::Result r = below(s, 100, 30, 0);
  TEST_ASSERT_FALSE(r.met);
  TEST_ASSERT_EQUAL_UINT32(kInterval, r.runS);
}

void test_a_gap_longer_than_two_and_a_half_intervals_breaks_the_run() {
  std::vector<Sample> s;
  addOn(s, 0, 70, 300);
  addOn(s, 69 * kInterval + 38, 3, 300);  // one gap of 38 s > 37.5 s
  TEST_ASSERT_EQUAL_UINT32(2 * kInterval, below(s, 100, 10, 0).runS);
  s.clear();
  addOn(s, 0, 70, 300);
  addOn(s, 69 * kInterval + 37, 3, 300);  // 37 s: one missed poll is tolerated
  TEST_ASSERT_EQUAL_UINT32(71 * kInterval + 37, below(s, 100, 10, 0, 73).runS);
}

// A monitor slower than its interval (40 s between the polls of this device, 15 s configured): the tolerance follows the
// real step, so the rule works instead of seeing every gap as a missed poll.
void test_a_slow_monitor_widens_the_tolerance_to_its_real_step() {
  std::vector<Sample> s;
  for (uint32_t i = 0; i < 20; ++i) s.push_back(on(i * 40, 300));
  TEST_ASSERT_EQUAL_UINT32(40, PowerRule::pollStep(s, kInterval));
  const PowerRule::Result r = below(s, 100, 600, 0);
  TEST_ASSERT_TRUE(r.met);
  TEST_ASSERT_EQUAL_UINT32(19 * 40, r.runS);
  // stale only after 2.5 real steps
  assertReason("met", PowerRule::evaluate(s, kCap, s.back().t + 99, kInterval, true, 100, 600, 0));
  assertReason("stale", PowerRule::evaluate(s, kCap, s.back().t + 101, kInterval, true, 100, 600, 0));
  // one missed poll (80 s) is tolerated, two (120 s) are not
  s.push_back(on(s.back().t + 80, 300));
  TEST_ASSERT_EQUAL_UINT32(19 * 40 + 80, below(s, 100, 600, 0).runS);
  s.push_back(on(s.back().t + 120, 300));
  TEST_ASSERT_EQUAL_UINT32(0, below(s, 100, 600, 0).runS);
}

void test_the_poll_step_is_never_below_the_interval_and_ignores_one_late_poll() {
  std::vector<Sample> s;
  for (uint32_t i = 0; i < 5; ++i) s.push_back(on(i * 5, 300));  // faster than configured: the interval stays
  TEST_ASSERT_EQUAL_UINT32(kInterval, PowerRule::pollStep(s, kInterval));
  s.clear();
  addOn(s, 0, 10, 300);
  s.push_back(on(s.back().t + 60, 300));  // one late poll among regular ones
  TEST_ASSERT_EQUAL_UINT32(kInterval, PowerRule::pollStep(s, kInterval));
  s.resize(2);  // too few samples to tell: the interval
  s[1].t = 100;
  TEST_ASSERT_EQUAL_UINT32(kInterval, PowerRule::pollStep(s, kInterval));
}

void test_after_a_restart_only_a_false_condition_arms_a_fired_rule() {
  TEST_ASSERT_TRUE(PowerRule::rearmsAfterRestart("not-met"));
  TEST_ASSERT_TRUE(PowerRule::rearmsAfterRestart("relay-off"));
  for (const char* r : {"no-data", "warmup", "waiting", "stale", "unanswered", "relay-unknown", "no-power-dp", "met"}) {
    TEST_ASSERT_FALSE_MESSAGE(PowerRule::rearmsAfterRestart(r), r);
  }
  TEST_ASSERT_FALSE(PowerRule::rearmsAfterRestart(nullptr));
  // The reasons the history gives right after a boot, while the socket keeps consuming below the threshold, never arm it.
  std::vector<Sample> s;
  TEST_ASSERT_FALSE(PowerRule::rearmsAfterRestart(below(s, 100, 30, 15).reason));  // no-data
  s.push_back(on(10, 300));
  TEST_ASSERT_FALSE(PowerRule::rearmsAfterRestart(below(s, 100, 30, 15).reason));  // warmup
  addOn(s, 25, 2, 300);
  TEST_ASSERT_FALSE(PowerRule::rearmsAfterRestart(below(s, 100, 30, 15).reason));  // waiting
  s.push_back(on(55, 2000));
  TEST_ASSERT_TRUE(PowerRule::rearmsAfterRestart(below(s, 100, 30, 15).reason));   // not-met: it went away
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_history_never_fires);
  RUN_TEST(test_an_old_last_answer_is_stale);
  RUN_TEST(test_the_last_answer_decides_the_reason);
  RUN_TEST(test_below_fires_after_for_s);
  RUN_TEST(test_above_is_the_mirror);
  RUN_TEST(test_a_clean_off_before_the_run_means_warm_up);
  RUN_TEST(test_a_run_from_the_start_of_the_history_gets_the_warm_up);
  RUN_TEST(test_a_full_ring_needs_no_warm_up);
  RUN_TEST(test_a_failed_answer_restarts_the_count);
  RUN_TEST(test_a_gap_longer_than_two_and_a_half_intervals_breaks_the_run);
  RUN_TEST(test_a_slow_monitor_widens_the_tolerance_to_its_real_step);
  RUN_TEST(test_the_poll_step_is_never_below_the_interval_and_ignores_one_late_poll);
  RUN_TEST(test_after_a_restart_only_a_false_condition_arms_a_fired_rule);
  return UNITY_END();
}
