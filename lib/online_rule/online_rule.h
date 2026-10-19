#pragma once
//
// The decision of the trigger "online" (a socket went offline / came online), apart from the firmware: plain C++, no Arduino,
// so it is tested on the computer (`pio test -e native`, tests/native/). scenario.cpp feeds it the device's state
// (Devices::State) every look.
//
// A trigger keeps a "confirmed" state of its socket: the state that has held for `for_s`. A change of the socket's state
// that holds for `for_s` becomes the confirmed one and is an event (if `to` wants it); one that goes back sooner is no event
// at all, and neither is the way back. Nothing counts before the socket has been seen online since the board started (or
// since the device was edited): a socket that comes up later than the board after a blackout, or one that is dead from the
// start, first answers into the base, with no event. A new, enabled again or edited trigger takes the current state as its
// base, with no event either.
//
#include <cstdint>

namespace OnlineRule {

enum class To : uint8_t { Offline, Online, Any };

// What the board knows of the socket now.
struct Device {
  bool everOnline = false;  // answered at least once since boot (or since the device was edited)
  bool online = false;      // with the hysteresis of the monitor (Devices::kOfflineAfterFails)
  uint32_t sinceMs = 0;     // millis() it became what it is now
};

// The memory of one trigger.
struct Watch {
  int8_t confirmed = -1;  // -1 none yet, 0 offline, 1 online
};

struct Look {
  int8_t fire = -1;                 // -1 nothing; 0 went offline, 1 came online: start the scenario
  const char* reason = "watching";  // watching | holding (changed, not for for_s yet) | not-seen-online
  uint32_t heldS = 0;               // how long the socket has been in its current state
};

Look look(Watch& w, const Device& d, uint32_t nowMs, uint32_t forS, To to);

}  // namespace OnlineRule
