// Host tests of the trigger "online" (lib/online_rule): pio test -e native
#include <unity.h>

#include "online_rule.h"

using OnlineRule::Device;
using OnlineRule::Look;
using OnlineRule::To;
using OnlineRule::Watch;

namespace {

Device dev(bool everOnline, bool online, uint32_t sinceMs) {
  Device d;
  d.known = true;
  d.everOnline = everOnline;
  d.online = online;
  d.sinceMs = sinceMs;
  return d;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_nothing_counts_before_the_first_query() {
  Watch w;
  Look l = OnlineRule::look(w, Device(), 3000, 0, To::Any);  // just booted: no query has finished yet
  TEST_ASSERT_EQUAL(-1, l.fire);
  TEST_ASSERT_EQUAL_STRING("not-seen-online", l.reason);
}

void test_a_socket_that_answers_the_first_query_is_the_base() {
  Watch w;  // the board was only restarted (OTA): the socket answers its first query
  OnlineRule::look(w, Device(), 3000, 0, To::Any);
  Look l = OnlineRule::look(w, dev(true, true, 6000), 6000, 0, To::Any);
  TEST_ASSERT_EQUAL(-1, l.fire);
  TEST_ASSERT_EQUAL_STRING("watching", l.reason);
  TEST_ASSERT_EQUAL(0, OnlineRule::look(w, dev(true, false, 90000), 90000, 0, To::Any).fire);  // and then a real change
}

void test_a_socket_offline_at_its_first_query_comes_online_as_an_event() {
  Watch w;  // plugged in later, or up later than the board after a blackout
  OnlineRule::look(w, Device(), 3000, 30, To::Online);
  Look l = OnlineRule::look(w, dev(false, false, 6000), 60000, 30, To::Online);
  TEST_ASSERT_EQUAL(-1, l.fire);
  TEST_ASSERT_EQUAL_STRING("not-seen-online", l.reason);
  l = OnlineRule::look(w, dev(true, true, 70000), 80000, 30, To::Online);
  TEST_ASSERT_EQUAL(-1, l.fire);  // not for for_s yet
  TEST_ASSERT_EQUAL_STRING("holding", l.reason);
  TEST_ASSERT_EQUAL(1, OnlineRule::look(w, dev(true, true, 70000), 100000, 30, To::Online).fire);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, true, 70000), 103000, 30, To::Online).fire);  // once
}

void test_a_socket_that_never_comes_up_gives_no_offline() {
  Watch w;
  OnlineRule::look(w, Device(), 3000, 0, To::Offline);
  for (uint32_t t = 6000; t < 3600000; t += 60000) {
    TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(false, false, 6000), t, 0, To::Offline).fire);
  }
}

void test_a_first_online_that_does_not_hold_is_no_event() {
  Watch w;
  OnlineRule::look(w, dev(false, false, 6000), 60000, 60, To::Any);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, true, 70000), 100000, 60, To::Any).fire);  // online 30 s
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, false, 110000), 200000, 60, To::Any).fire);  // gone again: still the base
}

void test_a_new_trigger_takes_the_current_state_as_its_base() {
  Watch w;
  Look l = OnlineRule::look(w, dev(true, false, 1000), 50000, 0, To::Offline);  // saved while the socket is offline
  TEST_ASSERT_EQUAL(-1, l.fire);
  TEST_ASSERT_EQUAL(49, l.heldS);
  l = OnlineRule::look(w, dev(true, true, 60000), 60000, 0, To::Any);
  TEST_ASSERT_EQUAL(1, l.fire);
}

void test_each_change_fires_once() {
  Watch w;
  OnlineRule::look(w, dev(true, true, 0), 1000, 0, To::Any);
  TEST_ASSERT_EQUAL(0, OnlineRule::look(w, dev(true, false, 5000), 6000, 0, To::Any).fire);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, false, 5000), 9000, 0, To::Any).fire);
  TEST_ASSERT_EQUAL(1, OnlineRule::look(w, dev(true, true, 10000), 12000, 0, To::Any).fire);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, true, 10000), 15000, 0, To::Any).fire);
}

void test_to_picks_the_direction_but_the_base_follows_every_change() {
  Watch w;
  OnlineRule::look(w, dev(true, true, 0), 1000, 0, To::Online);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, false, 2000), 3000, 0, To::Online).fire);  // offline: not wanted
  TEST_ASSERT_EQUAL(1, OnlineRule::look(w, dev(true, true, 4000), 6000, 0, To::Online).fire);    // back: wanted
  Watch o;
  OnlineRule::look(o, dev(true, true, 0), 1000, 0, To::Offline);
  TEST_ASSERT_EQUAL(0, OnlineRule::look(o, dev(true, false, 2000), 3000, 0, To::Offline).fire);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(o, dev(true, true, 4000), 6000, 0, To::Offline).fire);
}

void test_a_change_must_hold_for_s() {
  Watch w;
  OnlineRule::look(w, dev(true, true, 0), 1000, 60, To::Any);
  Look l = OnlineRule::look(w, dev(true, false, 10000), 40000, 60, To::Any);
  TEST_ASSERT_EQUAL(-1, l.fire);
  TEST_ASSERT_EQUAL_STRING("holding", l.reason);
  TEST_ASSERT_EQUAL(30, l.heldS);
  TEST_ASSERT_EQUAL(0, OnlineRule::look(w, dev(true, false, 10000), 70000, 60, To::Any).fire);
}

void test_a_short_loss_gives_no_event_and_no_way_back() {
  Watch w;
  OnlineRule::look(w, dev(true, true, 0), 1000, 300, To::Any);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, false, 10000), 200000, 300, To::Any).fire);  // 3 min 10 s offline
  Look l = OnlineRule::look(w, dev(true, true, 210000), 213000, 300, To::Any);                      // back
  TEST_ASSERT_EQUAL(-1, l.fire);
  TEST_ASSERT_EQUAL_STRING("watching", l.reason);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, true, 210000), 900000, 300, To::Any).fire);  // never an "online"
}

void test_an_edited_device_starts_again() {
  Watch w;
  OnlineRule::look(w, dev(true, true, 0), 1000, 0, To::Any);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, Device(), 4000, 0, To::Any).fire);  // the state was reset: no query yet
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(w, dev(true, true, 6000), 6000, 0, To::Any).fire);  // answered: the base again
  Watch v;
  OnlineRule::look(v, dev(true, true, 0), 1000, 0, To::Any);
  OnlineRule::look(v, Device(), 4000, 0, To::Any);
  TEST_ASSERT_EQUAL(-1, OnlineRule::look(v, dev(false, false, 5000), 6000, 0, To::Any).fire);  // its first query failed
  TEST_ASSERT_EQUAL(1, OnlineRule::look(v, dev(true, true, 9000), 9000, 0, To::Any).fire);    // so its first online counts
}

void test_millis_wrapping() {
  Watch w;
  OnlineRule::look(w, dev(true, true, 0xFFFFF000u), 0xFFFFF100u, 10, To::Any);
  Look l = OnlineRule::look(w, dev(true, false, 0xFFFFF800u), 0x00002000u, 10, To::Any);  // 10.2 s later, across the wrap
  TEST_ASSERT_EQUAL(0, l.fire);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_counts_before_the_first_query);
  RUN_TEST(test_a_socket_that_answers_the_first_query_is_the_base);
  RUN_TEST(test_a_socket_offline_at_its_first_query_comes_online_as_an_event);
  RUN_TEST(test_a_socket_that_never_comes_up_gives_no_offline);
  RUN_TEST(test_a_first_online_that_does_not_hold_is_no_event);
  RUN_TEST(test_a_new_trigger_takes_the_current_state_as_its_base);
  RUN_TEST(test_each_change_fires_once);
  RUN_TEST(test_to_picks_the_direction_but_the_base_follows_every_change);
  RUN_TEST(test_a_change_must_hold_for_s);
  RUN_TEST(test_a_short_loss_gives_no_event_and_no_way_back);
  RUN_TEST(test_an_edited_device_starts_again);
  RUN_TEST(test_millis_wrapping);
  return UNITY_END();
}
