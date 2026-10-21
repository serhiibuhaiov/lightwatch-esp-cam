#include "energy_meter.h"

namespace EnergyMeter {

void Meter::observe(uint64_t ms, int relay, int32_t powerRaw) {
  if (!started_) {
    started_ = true;
    firstMs_ = ms;
  }
  if (open_ && ms > lastMs_) {
    acc_ += static_cast<uint64_t>(held_) * (ms - lastMs_);
    coveredMs_ += ms - lastMs_;
  }
  if (relay == 0) {
    held_ = 0;  // off: 0 W, whatever the socket still reports
    open_ = true;
  } else if (powerRaw != kNone) {
    held_ = powerRaw > 0 ? static_cast<uint32_t>(powerRaw) : 0;
    open_ = true;
  } else {
    open_ = false;  // on (or unknown) with no power reading: nothing to hold
  }
  if (open_) everHeld_ = true;
  if (ms > lastMs_) lastMs_ = ms;  // two observers may race by a millisecond: never back
}

void Meter::lose(uint64_t ms) {
  (void)ms;  // the span since the last state is simply not counted
  open_ = false;
}

bool Meter::report(uint64_t ms, int32_t raw, bool pushed) {
  (void)ms;
  if (raw < 0 || raw == kNone) return false;
  ++reports_;
  const bool add = pushed || (haveLast_ && raw != last_);
  haveLast_ = haveAny_ = true;
  last_ = raw;
  if (add) {
    reported_ += static_cast<uint64_t>(raw);
    ++counted_;
  }
  return add;
}

double Meter::integratedWh(uint64_t nowMs) const {
  uint64_t a = acc_;
  if (open_ && nowMs > lastMs_) a += static_cast<uint64_t>(held_) * (nowMs - lastMs_);
  return static_cast<double>(a) / 36000000.0;  // 0.1 W x ms -> Wh
}

uint32_t Meter::sinceS(uint64_t nowMs) const {
  return started_ && nowMs > firstMs_ ? static_cast<uint32_t>((nowMs - firstMs_) / 1000) : 0;
}

uint32_t Meter::coveredS(uint64_t nowMs) const {
  uint64_t c = coveredMs_;
  if (open_ && nowMs > lastMs_) c += nowMs - lastMs_;
  return static_cast<uint32_t>(c / 1000);
}

}  // namespace EnergyMeter
