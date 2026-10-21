#pragma once
//
// The energy a socket has used since the board started, counted two ways side by side so they can be compared (nothing
// is stored across a restart). Plain C++, no Arduino: tested on the computer (`pio test -e native`, tests/native/).
//
// Integrated: the board's own sum of power x time. Every fresh state of the socket (an answer to a status query, or the
// data points it pushed on its own) gives the power that holds from then on, until the next one; a switch that is off
// means 0 W (a socket keeps reporting its old power for a while after it was switched off). Needs the power reading,
// not the socket's energy data point, so it works the same on every model. As exact as the power is fresh: a socket that
// pushes its changes (kept connection) is followed closely, one that is only polled is held for a whole poll. Time the
// socket was lost (counted offline, edited, ignored) is not counted: covered_s says how much of since_s was.
//
// Reported: the sum of what the socket's energy data point (add_ele, 0.001 kWh = 1 Wh) says. Its meaning differs between
// firmwares: the energy since the socket's last report (it starts over after each), or since it was switched on, over a
// window of ~30 minutes... A pushed value is a report of the socket and always counts; a value read by a status query
// counts only when it differs from the last one seen (the same report read again would count twice), and the very first
// one only sets the base (it may be mostly from before the board started). Two equal reports in a row that are only read
// by queries count once. A socket whose data point is a running total would sum to nonsense: compare with integrated.
//
#include <cstdint>

namespace EnergyMeter {

constexpr int32_t kNone = INT32_MIN;  // a reading that is not there

class Meter {
 public:
  // A fresh state of the socket at `ms` (any monotonic milliseconds): its switch (1 on, 0 off, -1 unknown) and its power
  // (raw 0.1 W as the socket sends it, kNone if the answer has none). Ends the span since the previous state (adding its
  // power x time) and holds this power from now on. Without a power that can be told (no reading and the switch not off),
  // nothing is held until the next state.
  void observe(uint64_t ms, int relay, int32_t powerRaw);
  // The socket was lost at `ms` (counted offline, edited, ignored): the time since the last state is not counted.
  void lose(uint64_t ms);
  // A value of the energy data point, raw 0.001 kWh (= Wh); pushed: sent by the socket on its own, else read by a query.
  // True if it was added to the reported sum.
  bool report(uint64_t ms, int32_t raw, bool pushed);
  // The socket was edited (its data points may mean something else now): the next value read by a query is a new base.
  void rebase() { haveLast_ = false; }

  bool started() const { return started_; }       // observed at least once
  bool counting() const { return everHeld_; }     // a power was held at least once: the integrated sum means something
  bool holding() const { return open_; }          // a power is being held now
  int32_t heldPowerRaw() const { return open_ ? static_cast<int32_t>(held_) : kNone; }  // 0.1 W
  // Integrated energy up to `nowMs` (the span still open included), in Wh.
  double integratedWh(uint64_t nowMs) const;
  // Seconds since the first state, and how many of them were counted (the open span included).
  uint32_t sinceS(uint64_t nowMs) const;
  uint32_t coveredS(uint64_t nowMs) const;

  bool reportedStarted() const { return haveAny_; }  // the energy data point was seen at least once
  uint64_t reportedWh() const { return reported_; }
  uint32_t reports() const { return reports_; }       // values seen (pushed or read)
  uint32_t reportsCounted() const { return counted_; }
  int32_t lastReportRaw() const { return haveAny_ ? last_ : kNone; }

 private:
  // Integrated: in 0.1 W x ms (2 kW for a year is ~6e14, far from the 64-bit end), so nothing is lost to rounding.
  uint64_t acc_ = 0;
  uint64_t coveredMs_ = 0;
  uint64_t firstMs_ = 0, lastMs_ = 0;
  uint32_t held_ = 0;  // 0.1 W
  bool started_ = false, open_ = false, everHeld_ = false;
  // Reported.
  uint64_t reported_ = 0;
  uint32_t reports_ = 0, counted_ = 0;
  int32_t last_ = 0;
  bool haveLast_ = false, haveAny_ = false;
};

}  // namespace EnergyMeter
