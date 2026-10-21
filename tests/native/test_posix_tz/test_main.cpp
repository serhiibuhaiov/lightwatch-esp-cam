// Host tests of the check of config time.tz (lib/posix_tz): pio test -e native
#include <unity.h>

#include "posix_tz.h"

namespace {

bool ok(const char* tz) {
  std::string why;
  return PosixTz::valid(tz, why);
}

std::string why(const char* tz) {
  std::string w;
  PosixTz::valid(tz, w);
  return w;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_strings_in_use_are_valid() {
  TEST_ASSERT_TRUE(ok("UTC0"));
  TEST_ASSERT_TRUE(ok("EET-2EEST,M3.5.0/3,M10.5.0/4"));  // Kyiv
  TEST_ASSERT_TRUE(ok("EET-2"));                         // Kyiv without the summer time
  TEST_ASSERT_TRUE(ok("CET-1CEST,M3.5.0,M10.5.0/3"));
  TEST_ASSERT_TRUE(ok("EST5EDT,M3.2.0/2:00:00,M11.1.0/2:00:00"));
  TEST_ASSERT_TRUE(ok("<+03>-3"));
  TEST_ASSERT_TRUE(ok("<-0330>3:30"));
  TEST_ASSERT_TRUE(ok("IST-5:30"));
  TEST_ASSERT_TRUE(ok("NZST-12NZDT,M9.5.0,M4.1.0/3"));
  TEST_ASSERT_TRUE(ok("ABC-2DEF-3,J60/2,300/-1"));
  TEST_ASSERT_TRUE(ok("EET-2EEST"));  // newlib's default rule
}

void test_typos_are_rejected_with_a_reason() {
  TEST_ASSERT_FALSE(ok(""));
  TEST_ASSERT_FALSE(ok("UTC"));                             // no offset
  TEST_ASSERT_FALSE(ok("Europe/Kyiv"));                     // an IANA name, not POSIX
  TEST_ASSERT_FALSE(ok("EE-2"));                            // name too short
  TEST_ASSERT_FALSE(ok("EET-25"));                          // offset hours
  TEST_ASSERT_FALSE(ok("EET-2:60"));                        // minutes
  TEST_ASSERT_FALSE(ok("EET-2EEST,M3.5.0/3"));              // one rule only
  TEST_ASSERT_FALSE(ok("EET-2EEST,M13.5.0/3,M10.5.0/4"));   // month
  TEST_ASSERT_FALSE(ok("EET-2EEST,M3.6.0/3,M10.5.0/4"));    // week
  TEST_ASSERT_FALSE(ok("EET-2EEST,M3.5.7/3,M10.5.0/4"));    // day
  TEST_ASSERT_FALSE(ok("EET-2EEST,M3.5.0/3,M10.5.0/4 "));   // trailing text
  TEST_ASSERT_FALSE(ok("EET-2EEST;M3.5.0/3,M10.5.0/4"));
  TEST_ASSERT_FALSE(ok("EET-2EEST,J0,J365"));               // J is 1..365
  TEST_ASSERT_FALSE(ok("<+3>-3"));                          // <...> needs 3+ characters
  TEST_ASSERT_FALSE(ok("<+03-3"));
  TEST_ASSERT_TRUE(why("UTC").find("offset") != std::string::npos);
  TEST_ASSERT_TRUE(why("EET-2EEST,M13.5.0/3,M10.5.0/4").find("month") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_strings_in_use_are_valid);
  RUN_TEST(test_typos_are_rejected_with_a_reason);
  return UNITY_END();
}
