#include "http_template.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace HttpTemplate {
namespace {

bool nameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

bool fieldOf(const std::string& s, Field& out) {
  static const struct {
    const char* name;
    Field field;
  } kFields[] = {{"power", Field::Power}, {"current", Field::Current}, {"voltage", Field::Voltage}, {"energy", Field::Energy},
                 {"relay", Field::Relay}, {"energy_integrated", Field::EnergyIntegrated}, {"energy_reported", Field::EnergyReported}};
  for (const auto& f : kFields) {
    if (s == f.name) {
      out = f.field;
      return true;
    }
  }
  return false;
}

// The inside of one {{...}}.
bool parseRef(const std::string& s, Ref& out) {
  if (s == "time") {
    out.kind = Ref::Kind::Time;
    return true;
  }
  if (s == "scenario") {
    out.kind = Ref::Kind::Scenario;
    return true;
  }
  if (s == "trigger") {
    out.kind = Ref::Kind::Trigger;
    return true;
  }
  const size_t dot = s.find('.');
  if (dot == std::string::npos || dot == 0 || dot > 32) return false;
  for (size_t i = 0; i < dot; ++i) {
    if (!nameChar(s[i])) return false;
  }
  if (!fieldOf(s.substr(dot + 1), out.field)) return false;
  out.device = s.substr(0, dot);
  out.kind = out.device == "when" ? Ref::Kind::When : Ref::Kind::Device;
  if (out.kind == Ref::Kind::When) out.device.clear();
  return true;
}

constexpr size_t kMaxInside = 56;  // longer than any placeholder (a 32-char name + ".energy_integrated"): "{{" without its "}}" nearby is not one

// The placeholder that starts at text[at] ("{{"): its length up to and with "}}" and what it names; 0 if none.
size_t refAt(const char* text, size_t n, size_t at, Ref& ref) {
  const size_t from = at + 2;
  for (size_t i = from; i + 1 < n && i - from <= kMaxInside; ++i) {
    if (text[i] == '}' && text[i + 1] == '}') return parseRef(std::string(text + from, i - from), ref) ? i + 2 - at : 0;
  }
  return 0;
}

// `raw` / 10^decimals as text: 1067, 1 -> "106.7"; -5, 3 -> "-0.005".
std::string decimal(int32_t raw, unsigned decimals) {
  uint32_t div = 1;
  for (unsigned i = 0; i < decimals; ++i) div *= 10;
  const int64_t v = raw;
  const uint64_t a = static_cast<uint64_t>(v < 0 ? -v : v);
  char buf[32];
  if (decimals == 0) {
    snprintf(buf, sizeof(buf), "%s%llu", v < 0 ? "-" : "", static_cast<unsigned long long>(a));
  } else {
    snprintf(buf, sizeof(buf), "%s%llu.%0*llu", v < 0 ? "-" : "", static_cast<unsigned long long>(a / div), static_cast<int>(decimals),
             static_cast<unsigned long long>(a % div));
  }
  return buf;
}

}  // namespace

bool validate(const char* text, size_t n, bool url, bool hasWhen, std::string& err) {
  size_t pathAt = n;  // a URL: where its path (or query) begins; no placeholder before it
  if (url) {
    const size_t scheme = std::string_view(text, n).find("://");
    pathAt = scheme == std::string_view::npos ? 0 : scheme + 3;
    while (pathAt < n && text[pathAt] != '/' && text[pathAt] != '?' && text[pathAt] != '#') ++pathAt;
  }
  for (size_t i = 0; i + 1 < n; ++i) {
    if (text[i] != '{' || text[i + 1] != '{') continue;
    Ref ref;
    const size_t len = refAt(text, n, i, ref);
    const std::string seen(text + i, std::min(n - i, len ? len : static_cast<size_t>(24)));
    if (!len) {
      err = "unknown placeholder at byte " + std::to_string(i) + " (" + seen +
            "...): use {{device.power|current|voltage|energy|relay|energy_integrated|energy_reported}}, {{when.<field>}}, {{time}}, "
            "{{scenario}} or {{trigger}}";
      return false;
    }
    if (url && i < pathAt) {
      err = "placeholder " + seen + " in the host: only the path and the query may hold placeholders";
      return false;
    }
    if (ref.kind == Ref::Kind::When && !hasWhen) {
      err = "placeholder " + seen + ": only a scenario whose trigger names a device (power_below, power_above, relay, online) has a `when` device";
      return false;
    }
    i += len - 1;
  }
  return true;
}

std::string format(Field field, int32_t raw) {
  if (raw == kNone) return "-";
  switch (field) {
    case Field::Power: return decimal(raw, 1);    // 0.1 W -> W
    case Field::Voltage: return decimal(raw, 1);  // 0.1 V -> V
    case Field::Current: return decimal(raw, 3);  // mA -> A
    case Field::Energy: return decimal(raw, 3);   // 0.001 kWh -> kWh
    case Field::Relay: return raw == 1 ? "on" : raw == 0 ? "off" : "-";
    case Field::EnergyIntegrated:
    case Field::EnergyReported: return decimal(raw, 3);  // Wh -> kWh
  }
  return "-";
}

bool expand(const char* text, size_t n, const Resolve& resolve, size_t maxBytes, std::string& out) {
  out.clear();
  size_t done = 0;  // text[done..] is not in `out` yet
  for (size_t i = 0; i + 1 < n; ++i) {
    if (text[i] != '{' || text[i + 1] != '{') continue;
    Ref ref;
    const size_t len = refAt(text, n, i, ref);
    if (!len) continue;
    out.append(text + done, i - done);
    out += resolve(ref);
    if (out.size() > maxBytes) return false;
    done = i + len;
    i = done - 1;
  }
  out.append(text + done, n - done);
  return out.size() <= maxBytes;
}

}  // namespace HttpTemplate
