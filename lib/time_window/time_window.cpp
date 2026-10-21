#include "time_window.h"

namespace TimeWindow {

bool Window::contains(int minute) const {
  if (!set()) return true;
  return from < to ? minute >= from && minute < to : minute >= from || minute < to;
}

bool parseClock(const char* t, int16_t& minute) {
  if (!t) return false;
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (!digit(t[0]) || !digit(t[1]) || t[2] != ':' || !digit(t[3]) || !digit(t[4]) || t[5] != '\0') return false;
  const int h = (t[0] - '0') * 10 + (t[1] - '0');
  const int m = (t[3] - '0') * 10 + (t[4] - '0');
  if (h > 23 || m > 59) return false;
  minute = static_cast<int16_t>(h * 60 + m);
  return true;
}

std::string clockText(int minute) {
  char buf[8];
  const int h = (minute / 60) % 24, m = minute % 60;
  buf[0] = static_cast<char>('0' + h / 10);
  buf[1] = static_cast<char>('0' + h % 10);
  buf[2] = ':';
  buf[3] = static_cast<char>('0' + m / 10);
  buf[4] = static_cast<char>('0' + m % 10);
  buf[5] = '\0';
  return buf;
}

std::string text(const Window& w) { return w.set() ? clockText(w.from) + "-" + clockText(w.to) : std::string(); }

}  // namespace TimeWindow
