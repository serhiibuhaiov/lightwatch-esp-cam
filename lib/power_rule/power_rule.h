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
// than 2.5 poll steps (at least 20 s), for `forS` seconds. The poll step is `intervalS`, or the real step when the
// monitor is slower than that (a cycle over many, slow or dead devices takes longer than the interval): the median gap
// between the newest answers (see pollStep()). A run that starts right after a clean "relay off" answer,
// or with the history itself while the ring is not full yet (after a boot: the power came back), gets `warmupS` first.
Result evaluate(const std::vector<Sample>& samples, size_t capacity, uint32_t now, uint32_t intervalS, bool below,
                uint32_t watts, uint32_t forS, uint32_t warmupS);

// The step between the polls of a device: `intervalS`, or the median of the last (up to 8) gaps between its samples
// when that is longer. The median ignores a single late or missed poll; samples are written for failed polls too, so
// only a slow monitor (or polls skipped while a command had the device) makes the gaps longer.
uint32_t pollStep(const std::vector<Sample>& samples, uint32_t intervalS);

// A rule that has fired is armed again only by a look that shows its condition false for sure: the power on the other side
// of the threshold ("not-met") or the relay off ("relay-off"). Everything else says nothing about the condition: an
// unanswered query or a lost connection ("unanswered", "stale"), the history filling up after a boot or a gap ("no-data",
// "warmup", "waiting"). Arming on those fired the rule a second time for the same condition (a boiler that stayed idle got
// a second "ready" message after a short loss of its socket). The same holds for a rule restored "fired" after a restart.
bool rearms(const char* reason);

}  // namespace PowerRule
