#include "online_rule.h"

namespace OnlineRule {

Look look(Watch& w, const Device& d, uint32_t nowMs, uint32_t forS, To to) {
  Look l;
  if (!d.everOnline) {
    w.confirmed = -1;
    l.reason = "not-seen-online";
    return l;
  }
  const uint32_t heldMs = nowMs - d.sinceMs;
  l.heldS = heldMs / 1000;
  const int8_t now = d.online ? 1 : 0;
  if (w.confirmed < 0 || w.confirmed == now) {  // the base (first look since online), or nothing changed
    w.confirmed = now;
    return l;
  }
  if (heldMs < forS * 1000UL) {
    l.reason = "holding";
    return l;
  }
  w.confirmed = now;  // the base of the next change too, whether `to` wants this one or not
  if (to == To::Any || (to == To::Online) == d.online) l.fire = now;
  return l;
}

}  // namespace OnlineRule
