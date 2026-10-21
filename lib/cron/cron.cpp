#include "cron.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace Cron {
namespace {

struct Field {
  const char* name;
  int lo, hi;
  const char* const* names;  // 3-letter names for lo.. (nullptr: none)
};

const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC", nullptr};
const char* const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT", nullptr};

const Field kFields[5] = {{"minute", 0, 59, nullptr},
                          {"hour", 0, 23, nullptr},
                          {"day of month", 1, 31, nullptr},
                          {"month", 1, 12, kMonths},
                          {"day of week", 0, 7, kDays}};

bool value(const char*& p, const Field& f, int& out) {
  if (std::isdigit(static_cast<unsigned char>(*p))) {
    int v = 0, n = 0;
    while (std::isdigit(static_cast<unsigned char>(*p)) && n < 3) {
      v = v * 10 + (*p++ - '0');
      ++n;
    }
    if (std::isdigit(static_cast<unsigned char>(*p))) return false;
    out = v;
    return true;
  }
  if (!f.names) return false;
  for (int i = 0; f.names[i]; ++i) {
    bool same = true;
    for (int k = 0; k < 3; ++k) same = same && std::toupper(static_cast<unsigned char>(p[k])) == f.names[i][k];
    if (same && !std::isalpha(static_cast<unsigned char>(p[3]))) {
      p += 3;
      out = f.lo + i;
      return true;
    }
  }
  return false;
}

// One field, its text in [p, end): sets bits lo..hi of `bits`.
bool field(const char* p, const char* end, const Field& f, uint64_t& bits, bool& star, std::string& why) {
  bits = 0;
  star = *p == '*';
  const std::string bad = std::string("the ") + f.name + " field must be *, n, a-b, */s or a-b/s (lists with commas), values " +
                          std::to_string(f.lo) + ".." + std::to_string(f.hi);
  while (p < end) {
    int a = f.lo, b = f.hi, step = 1;
    if (*p == '*') {
      ++p;
    } else {
      if (!value(p, f, a)) {
        why = bad;
        return false;
      }
      b = a;
      if (*p == '-') {
        ++p;
        if (!value(p, f, b)) {
          why = bad;
          return false;
        }
      }
    }
    const bool single = a == b && *p == '/';
    if (*p == '/') {
      ++p;
      if (!value(p, Field{f.name, 1, f.hi, nullptr}, step) || step < 1) {
        why = std::string("the ") + f.name + " field: the step after / must be a number 1.." + std::to_string(f.hi);
        return false;
      }
      if (single) b = f.hi;  // n/s: from n to the end
    }
    if (a < f.lo || b > f.hi || a > b) {
      why = std::string("the ") + f.name + " field: values must be " + std::to_string(f.lo) + ".." + std::to_string(f.hi) +
            (a > b ? " and a range must go up (a-b with a <= b)" : "");
      return false;
    }
    for (int v = a; v <= b; v += step) bits |= uint64_t{1} << v;
    if (p < end) {
      if (*p != ',' || p + 1 >= end) {
        why = bad;
        return false;
      }
      ++p;
    }
  }
  return true;
}

bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int daysIn(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : d[m - 1];
}

bool dayMatches(const Expr& e, int y, int m, int d) {
  if (!(e.months >> m & 1)) return false;
  const bool dom = e.days >> d & 1, dow = e.weekdays >> weekday(y, m, d) & 1;
  return e.dayStar || e.weekdayStar ? dom && dow : dom || dow;
}

}  // namespace

bool parse(const char* text, Expr& out, std::string& why) {
  if (!text || std::strlen(text) > kMaxTextBytes) {
    why = "must be at most " + std::to_string(kMaxTextBytes) + " characters";
    return false;
  }
  const char* p = text;
  Expr e;
  for (int i = 0; i < 5; ++i) {
    while (*p == ' ' || *p == '\t') ++p;
    const char* s = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (s == p) {
      why = "needs 5 fields: minute hour day-of-month month day-of-week, e.g. \"0 21 * * *\"";
      return false;
    }
    uint64_t bits = 0;
    bool star = false;
    if (!field(s, p, kFields[i], bits, star, why)) return false;
    switch (i) {
      case 0: e.minutes = bits; break;
      case 1: e.hours = static_cast<uint32_t>(bits); break;
      case 2:
        e.days = static_cast<uint32_t>(bits);
        e.dayStar = star;
        break;
      case 3: e.months = static_cast<uint16_t>(bits); break;
      default:
        e.weekdays = static_cast<uint8_t>((bits | bits >> 7) & 0x7f);  // 7 is Sunday too
        e.weekdayStar = star;
    }
  }
  while (*p == ' ' || *p == '\t') ++p;
  if (*p) {
    why = "has more than 5 fields (minute hour day-of-month month day-of-week)";
    return false;
  }
  out = e;
  return true;
}

int weekday(int y, int m, int d) {  // days from civil (H. Hinnant), 1970-01-01 was a Thursday
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = static_cast<long>(era) * 146097 + doe - 719468;
  return static_cast<int>(((days % 7) + 11) % 7);  // 0 = Sunday
}

bool matches(const Expr& e, const Civil& t) {
  return (e.minutes >> t.minute & 1) && (e.hours >> t.hour & 1) && dayMatches(e, t.year, t.month, t.day);
}

bool next(const Expr& e, const Civil& from, Civil& out, int maxDays) {
  int y = from.year, m = from.month, d = from.day;
  int h0 = from.hour, min0 = from.minute;
  for (int i = 0; i <= maxDays; ++i) {
    if (dayMatches(e, y, m, d)) {
      for (int h = h0; h < 24; ++h) {
        if (!(e.hours >> h & 1)) continue;
        for (int mi = h == h0 ? min0 : 0; mi < 60; ++mi) {
          if (!(e.minutes >> mi & 1)) continue;
          out = Civil{y, m, d, h, mi};
          return true;
        }
      }
    }
    h0 = min0 = 0;
    if (++d > daysIn(y, m)) {
      d = 1;
      if (++m > 12) {
        m = 1;
        ++y;
      }
    }
  }
  return false;
}

std::string text(const Civil& t) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
  return buf;
}

}  // namespace Cron
