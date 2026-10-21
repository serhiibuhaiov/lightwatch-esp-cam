// Host tests of the hours of steps and triggers (lib/time_window): pio test -e native
#include <unity.h>

#include "time_window.h"

using TimeWindow::Window;

namespace {

Window w(const char* from, const char* to) {
  Window x;
  TEST_ASSERT_TRUE(TimeWindow::parseClock(from, x.from));
  TEST_ASSERT_TRUE(TimeWindow::parseClock(to, x.to));
  return x;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_clock_text_is_read_exactly() {
  int16_t m = -1;
  TEST_ASSERT_TRUE(TimeWindow::parseClock("05:00", m));
  TEST_ASSERT_EQUAL(300, m);
  TEST_ASSERT_TRUE(TimeWindow::parseClock("23:59", m));
  TEST_ASSERT_EQUAL(1439, m);
  TEST_ASSERT_TRUE(TimeWindow::parseClock("00:00", m));
  TEST_ASSERT_EQUAL(0, m);
  const char* bad[] = {"5:00", "24:00", "12:60", "12-00", "12:00 ", "", "ab:cd", "12:0", "112:00"};
  for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(TimeWindow::parseClock(b, m), b);
  TEST_ASSERT_FALSE(TimeWindow::parseClock(nullptr, m));
  TEST_ASSERT_EQUAL_STRING("05:07", TimeWindow::clockText(307).c_str());
  TEST_ASSERT_EQUAL_STRING("05:00-21:00", TimeWindow::text(w("05:00", "21:00")).c_str());
}

void test_from_is_in_to_is_out() {
  const Window x = w("05:00", "21:00");
  TEST_ASSERT_FALSE(x.contains(4 * 60 + 59));
  TEST_ASSERT_TRUE(x.contains(5 * 60));
  TEST_ASSERT_TRUE(x.contains(20 * 60 + 59));
  TEST_ASSERT_FALSE(x.contains(21 * 60));
  TEST_ASSERT_FALSE(x.contains(0));
}

void test_a_window_over_midnight() {
  const Window x = w("22:00", "06:00");
  TEST_ASSERT_TRUE(x.contains(22 * 60));
  TEST_ASSERT_TRUE(x.contains(23 * 60 + 59));
  TEST_ASSERT_TRUE(x.contains(0));
  TEST_ASSERT_TRUE(x.contains(5 * 60 + 59));
  TEST_ASSERT_FALSE(x.contains(6 * 60));
  TEST_ASSERT_FALSE(x.contains(12 * 60));
  TEST_ASSERT_FALSE(x.contains(21 * 60 + 59));
}

void test_no_window_lets_everything_through() {
  Window x;
  TEST_ASSERT_FALSE(x.set());
  TEST_ASSERT_TRUE(x.contains(0));
  TEST_ASSERT_TRUE(x.contains(1439));
  TEST_ASSERT_EQUAL(10, x.clockWaitMin);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clock_text_is_read_exactly);
  RUN_TEST(test_from_is_in_to_is_out);
  RUN_TEST(test_a_window_over_midnight);
  RUN_TEST(test_no_window_lets_everything_through);
  return UNITY_END();
}
