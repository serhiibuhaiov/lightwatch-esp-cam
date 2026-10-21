#include "posix_tz.h"

#include <cctype>

namespace PosixTz {
namespace {

bool isAlpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// A zone name: 3+ letters, or <...> with letters, digits, + and - (3+ of them).
bool name(const char*& p, const char* what, std::string& why) {
  if (*p == '<') {
    const char* s = ++p;
    while (isAlpha(*p) || isDigit(*p) || *p == '+' || *p == '-') ++p;
    if (*p != '>' || p - s < 3) {
      why = std::string(what) + " name in <...> must have 3 or more letters, digits, + or -, and end with >";
      return false;
    }
    ++p;
    return true;
  }
  const char* s = p;
  while (isAlpha(*p)) ++p;
  if (p - s < 3) {
    why = std::string(what) + " name must be 3 or more letters (or <...>), e.g. UTC or EET";
    return false;
  }
  return true;
}

// Digits as a number, at most `maxDigits` of them; false when there are none.
bool number(const char*& p, int maxDigits, int& out) {
  if (!isDigit(*p)) return false;
  out = 0;
  for (int i = 0; i < maxDigits && isDigit(*p); ++i) out = out * 10 + (*p++ - '0');
  return !isDigit(*p);
}

// [+|-]hh[:mm[:ss]] with hh <= maxHours.
bool clock(const char*& p, int maxHours, const char* what, std::string& why) {
  if (*p == '+' || *p == '-') ++p;
  int v = 0;
  if (!number(p, 3, v) || v > maxHours) {
    why = std::string(what) + " must be [+|-]hh[:mm[:ss]] with hh 0.." + std::to_string(maxHours);
    return false;
  }
  for (int part = 0; part < 2 && *p == ':'; ++part) {
    ++p;
    if (!number(p, 2, v) || v > 59) {
      why = std::string(what) + ": minutes and seconds must be 00..59";
      return false;
    }
  }
  return true;
}

// Jn, n or Mm.w.d.
bool date(const char*& p, const char* what, std::string& why) {
  int v = 0;
  if (*p == 'J') {
    ++p;
    if (!number(p, 3, v) || v < 1 || v > 365) {
      why = std::string(what) + ": Jn needs n 1..365";
      return false;
    }
    return true;
  }
  if (*p == 'M') {
    ++p;
    int m = 0, w = 0, d = 0;
    const bool ok = number(p, 2, m) && *p++ == '.' && number(p, 1, w) && *p++ == '.' && number(p, 1, d);
    if (!ok || m < 1 || m > 12 || w < 1 || w > 5 || d > 6) {
      why = std::string(what) + " must be Mm.w.d: month 1..12, week 1..5 (5 = the last), day 0..6 (0 = Sunday)";
      return false;
    }
    return true;
  }
  if (!number(p, 3, v) || v > 365) {
    why = std::string(what) + " must be Mm.w.d, Jn or n (0..365)";
    return false;
  }
  return true;
}

bool rule(const char*& p, const char* what, std::string& why) {
  if (!date(p, what, why)) return false;
  if (*p == '/') {
    ++p;
    if (!clock(p, 167, (std::string(what) + " time").c_str(), why)) return false;
  }
  return true;
}

}  // namespace

bool valid(const char* tz, std::string& why) {
  const char* p = tz ? tz : "";
  if (*p == '\0') {
    why = "is empty (UTC0 for UTC)";
    return false;
  }
  if (!name(p, "the standard time", why)) return false;
  if (*p == '\0') {
    why = "needs an offset after the name, e.g. UTC0 or EET-2";
    return false;
  }
  if (!clock(p, 24, "the offset", why)) return false;
  if (*p == '\0') return true;
  if (!name(p, "the summer time", why)) return false;
  if (*p != '\0' && *p != ',') {
    if (!clock(p, 24, "the summer time offset", why)) return false;
  }
  if (*p == '\0') return true;  // newlib's default rule
  if (*p++ != ',') {
    why = "the summer time rules must follow a comma: ,start[/time],end[/time]";
    return false;
  }
  if (!rule(p, "the start of the summer time", why)) return false;
  if (*p++ != ',') {
    why = "needs both rules: ,start[/time],end[/time]";
    return false;
  }
  if (!rule(p, "the end of the summer time", why)) return false;
  if (*p != '\0') {
    why = "unexpected text after the rules";
    return false;
  }
  return true;
}

}  // namespace PosixTz
