#pragma once
//
// The decision of a power rule (power_below / power_above), apart from the firmware: plain C++, no Arduino, so it is
// tested on the computer (`pio test -e native`, tests/native/). Devices::checkPower() feeds it the history of a device.
//
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PowerRule {

constexpr int32_t kNone = INT32_MIN;  // a reading the answer did not carry

// One finished status query of a device.
struct Sample {
  enum : uint8_t { kOk = 1, kRelayKnown = 2, kRelayOn = 4 };
  uint32_t t = 0;                                // uptime seconds
  int32_t v[4] = {kNone, kNone, kNone, kNone};  // raw power (0.1 W), current, voltage, energy
  uint8_t flags = 0;
};

struct Result {
  bool met = false;
  const char* reason = "no-data";  // met | waiting | warmup | not-met | relay-off | relay-unknown | unanswered | stale |
                                   // no-data | no-power-dp
  int32_t lastPower = kNone;       // the newest raw power reading (0.1 W)
  uint32_t runS = 0;               // how long the rule has held (after the warm-up)
};

// `samples`: oldest first, at most `capacity` (the history ring; full = the oldest ones are gone). `now`: uptime seconds.
// The newest answers must all be on the right side of `watts`, the relay on, with no unanswered query and no gap longer
// than 2.5 poll intervals (at least 20 s), for `forS` seconds. A run that starts right after a clean "relay off" answer,
// or with the history itself while the ring is not full yet (after a boot: the power came back), gets `warmupS` first.
Result evaluate(const std::vector<Sample>& samples, size_t capacity, uint32_t now, uint32_t intervalS, bool below,
                uint32_t watts, uint32_t forS, uint32_t warmupS);

}  // namespace PowerRule
