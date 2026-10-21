#pragma once
//
// Placeholders in the url and the body of an HTTP step, apart from the firmware: plain C++, no Arduino, so it is tested on
// the computer (`pio test -e native`, tests/native/). The board resolves the values (scenario.cpp, BoardEnv::submitHttp).
//
//   {{heater1.power}}  a reading of the socket named heater1, by name, not data point number (the device's meters say
//                      which data point carries it): power W, voltage V (one decimal), current A, energy kWh (three
//                      decimals); relay: on | off; energy_integrated, energy_reported: the energy since the board started,
//                      kWh (three decimals), counted two ways (lib/energy_meter): from the power over time, and as the
//                      sum of the socket's own energy reports
//   {{when.power}}     the same of the socket in the scenario's `when` (power_below, power_above, relay, online only)
//   {{time}}           the local time (time.tz) as 2026-10-19T14:05:00
//   {{scenario}}       the scenario's name;  {{trigger}}: what started the run (relay-off, power-below, manual...)
//
// A value that is not there (the socket offline, not answered yet, ignored, without that reading, not in the list; no
// clock) is "-". Every value is made of a-z 0-9 . : T _ -, so it needs no escaping, neither in a URL nor in a JSON string.
// Every "{{" must start a placeholder: an unknown one is an error when the scenario is saved (a typo is caught there).
// None in the scheme and host of a URL: the log shows the host.
//
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace HttpTemplate {

constexpr int32_t kNone = INT32_MIN;  // a reading that is not there

enum class Field : uint8_t { Power, Current, Voltage, Energy, Relay, EnergyIntegrated, EnergyReported };

// What one placeholder names.
struct Ref {
  enum class Kind : uint8_t { Device, When, Time, Scenario, Trigger };
  Kind kind = Kind::Time;
  std::string device;          // Kind::Device: the socket's name
  Field field = Field::Power;  // Kind::Device and Kind::When
};

// Checks every placeholder of `text` (n bytes). `url`: none may come before the path. `hasWhen`: the scenario's trigger
// names a socket, so {{when.*}} is allowed. False with `err` (what and where) otherwise.
bool validate(const char* text, size_t n, bool url, bool hasWhen, std::string& err);

// The text of a reading: `raw` as the socket sends it (0.1 W, mA, 0.1 V, 0.001 kWh; for Relay 1 on, 0 off, anything
// else unknown; for the energy since boot Wh), kNone for none -> "-".
std::string format(Field field, int32_t raw);

// `text` with every placeholder replaced by resolve(ref). False when the result would be longer than `maxBytes` (`out`
// is then incomplete). A placeholder that does not parse (not validated) is kept as it is.
using Resolve = std::function<std::string(const Ref&)>;
bool expand(const char* text, size_t n, const Resolve& resolve, size_t maxBytes, std::string& out);

}  // namespace HttpTemplate
