#pragma once
//
// Checks a POSIX TZ string (config time.tz) before it is used: newlib takes anything and quietly falls back to UTC on
// a string it cannot read, so a typo would go unnoticed. Plain C++, host tests in tests/native/test_posix_tz.
//
//   std offset [dst [offset] [,start[/time],end[/time]]]
//   std, dst: 3+ letters, or <...> (letters, digits, + and -), e.g. "EET", "<+03>"
//   offset:   [+|-]hh[:mm[:ss]], hh 0..24 (west of Greenwich is positive: "EET-2" is UTC+2)
//   start/end: Jn (1..365, no Feb 29), n (0..365), Mm.w.d (month 1..12, week 1..5, day 0..6, 0 = Sunday)
//   time:     [+|-]hh[:mm[:ss]], hh 0..167 (the extension newlib and glibc accept)
//
// Examples: "UTC0", "EET-2EEST,M3.5.0/3,M10.5.0/4" (Kyiv), "<+03>-3".
//
#include <string>

namespace PosixTz {

// True when `tz` is a well-formed POSIX TZ string; otherwise `why` says what is wrong.
bool valid(const char* tz, std::string& why);

}  // namespace PosixTz
