#include "power_rule.h"

#include <algorithm>

namespace PowerRule {

Result evaluate(const std::vector<Sample>& s, size_t capacity, uint32_t now, uint32_t intervalS, bool below, uint32_t watts,
                uint32_t forS, uint32_t warmupS) {
  Result pc;
  if (s.empty()) {
    pc.reason = "no-data";
    return pc;
  }
  const uint32_t maxGap = std::max<uint32_t>(20, intervalS * 5 / 2);  // one missed poll is tolerated, two are not
  const Sample& last = s.back();
  if (now - last.t > maxGap) {
    pc.reason = "stale";
    return pc;
  }
  if (!(last.flags & Sample::kOk)) {
    pc.reason = "unanswered";
    return pc;
  }
  pc.lastPower = last.v[0];
  if (!(last.flags & Sample::kRelayKnown)) {
    pc.reason = "relay-unknown";
    return pc;
  }
  if (!(last.flags & Sample::kRelayOn)) {
    pc.reason = "relay-off";
    return pc;
  }
  if (last.v[0] == kNone) {
    pc.reason = "no-power-dp";
    return pc;
  }
  const int64_t limit = static_cast<int64_t>(watts) * 10;  // raw power is in 0.1 W
  auto qualifies = [&](const Sample& x) {
    return (x.flags & Sample::kOk) && (x.flags & Sample::kRelayKnown) && (x.flags & Sample::kRelayOn) && x.v[0] != kNone &&
           (below ? x.v[0] < limit : x.v[0] > limit);
  };
  if (!qualifies(last)) {
    pc.reason = "not-met";  // the power is on the other side of the threshold
    return pc;
  }
  size_t i = s.size() - 1;  // the start of the current run: back while the answers qualify and follow each other closely
  while (i > 0 && qualifies(s[i - 1]) && s[i].t - s[i - 1].t <= maxGap) --i;
  uint32_t start = s[i].t;
  // The relay was switched on just before the run began (the answer before it is a clean "off"): warm-up first. A run
  // that begins with the history itself (after a boot, i.e. the power came back, or after the device was edited) does
  // not show what was before: it counts as just switched on too. Once the ring is full, the run is older than that.
  const Sample* before = i > 0 ? &s[i - 1] : nullptr;
  const bool switchedOn = before ? (before->flags & Sample::kOk) && (before->flags & Sample::kRelayKnown) &&
                                       !(before->flags & Sample::kRelayOn) && s[i].t - before->t <= maxGap
                                 : s.size() < capacity;
  if (switchedOn) start += warmupS;
  pc.runS = last.t > start ? last.t - start : 0;
  pc.met = pc.runS >= forS;
  pc.reason = pc.met ? "met" : last.t < start ? "warmup" : "waiting";
  return pc;
}

}  // namespace PowerRule
