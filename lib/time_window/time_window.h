#pragma once
//
// The hours of a step or of a scenario's trigger: "hours": {"from": "05:00", "to": "21:00", "clock_wait_min": 10}.
// Local time (config time.tz), minute resolution; `from` is in, `to` is out; from > to goes over midnight ("22:00"-"06:00").
// `clock_wait_min`: with no clock yet (no RTC; after a power cut the time comes with NTP), how long to wait for it before
// the step or the run is skipped. Plain C++, host tests in tests/native/test_time_window.
//
#include <cstdint>
#include <string>

namespace TimeWindow {

constexpr uint16_t kDefaultClockWaitMin = 10;
constexpr uint16_t kMaxClockWaitMin = 1440;

struct Window {
  int16_t from = -1, to = -1;  // minutes of the day 0..1439; -1: no window
  uint16_t clockWaitMin = kDefaultClockWaitMin;

  bool set() const { return from >= 0 && to >= 0; }
  // Is `minute` (0..1439, local) inside? Always true without a window.
  bool contains(int minute) const;
};

// "05:00" -> 300. False unless exactly HH:MM with HH 00..23 and MM 00..59.
bool parseClock(const char* text, int16_t& minute);
std::string clockText(int minute);  // 300 -> "05:00"
std::string text(const Window& w);  // "05:00-21:00"

}  // namespace TimeWindow
