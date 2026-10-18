#pragma once
//
// Devices: the list of Tuya devices, read from /devices.json on the SD card.
//
// This file is deliberately NOT part of config.json and is NEVER copied to the
// internal flash: it holds the local keys of the devices. Without a card there are
// no devices. The firmware writes it only from save() (the "configure devices" page: atomic
// write, the previous version kept as /devices.bak) and as an empty template when it does not
// exist; it can also be edited on a PC and re-read with load(). The keys are never sent out.
//
//   {
//     "version": 1,
//     "devices": [
//       { "name": "towel", "ip": "192.168.8.185", "key": "0123456789abcdef", "version": "3.4", "dp": 1,
//         "meters": { "power": 19, "current": 18, "voltage": 20, "energy": 17 } }
//     ]
//   }
//
// name: 1..32 chars of a-z 0-9 _ - (used in URLs), unique. key: exactly 16 characters.
// version: "3.4" or "3.5". dp: the on/off data point, default 1.
// meters (optional): which data point carries the power / current / voltage / energy reading, 0..255;
// 0 = the socket has none, absent = the usual Tuya socket numbers (19, 18, 20, 17). They label the readings
// on the page, fill the history, feed the power rules and say which data points UPDATEDPS re-reads;
// different socket models number them differently.
//
// State and monitor: a background task polls every device each monitor.interval_s (up to
// monitor.retries attempts of monitor.timeout_ms) and keeps the last answer here, so the web
// page and API read the state without touching the devices. Any status query a user starts
// updates it too. "online" means the last status query succeeded.
//
// Thread safety: every function takes one short internal mutex and copies; no I/O under it.
//
#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "tuya.h"

namespace Devices {

struct Device {
  String name;
  String ip;
  String key;
  Tuya::Version version = Tuya::Version::V34;
  uint16_t dp = 1;
  int16_t dpPower = -1, dpCurrent = -1, dpVoltage = -1, dpEnergy = -1;  // -1 = standard number, 0 = none
  bool ignore = false;  // not polled by the monitor and skipped by scenario steps (manual actions still work)
  bool keepConnected = false;  // one connection kept open for every operation (see Tuya::Target::keep)
  // UPDATEDPS before the poll while a power rule watches it. false: plain status queries only (a socket that keeps its
  // readings fresh by itself, or that misbehaves on UPDATEDPS: the boiler switched itself off during one).
  bool updatedps = true;

  Tuya::Target target() const {
    Tuya::Target t;
    t.ip = ip;
    t.key = key;
    t.version = version;
    t.keep = keepConnected;
    return t;
  }
};

struct LoadInfo {
  bool sd = false;          // the card was mounted
  bool fileFound = false;   // /devices.json exists
  bool templateCreated = false;
  size_t count = 0;         // devices in the active list
  String error;             // why the last load() did not take the file ("" = fine)
};

// "online" has a hysteresis: a device that was online is only called offline after kOfflineAfterFails failed status
// queries in a row (polls, one per monitor cycle, and user queries count alike). A device that was never queried or was
// offline needs no such confirmation: its first failure keeps it offline. One success makes it online again.
constexpr uint8_t kOfflineAfterFails = 2;

struct State {
  bool known = false;       // at least one status query has finished
  bool online = false;      // see kOfflineAfterFails; fails > 0 while online means "did not answer, not yet offline"
  uint32_t lastOkMs = 0;    // millis() of the last success (0 = never)
  uint32_t lastTryMs = 0;   // millis() of the last query, successful or not
  uint32_t fails = 0;       // consecutive failed status queries
  uint32_t latencyMs = 0;   // duration of the last successful query
  String error;             // last failure: code ...
  String detail;            // ... and explanation, "" while online
  String dpsJson;           // the "dps" object of the last success, as JSON text ("" = none yet)
};

// Reads /devices.json. A valid file replaces the list; an invalid one is reported in
// info().error and the previous list stays. Call from setup() after Storage::begin(),
// and again to reload after editing the file.
void load();

struct SaveResult {
  bool ok = false;
  bool invalid = false;   // the document was rejected (HTTP 400), see error
  bool noSd = false;      // no card (HTTP 503)
  size_t count = 0;       // devices saved
  String error;
};

// The device list for the editor: everything but the keys (only key_set).
String configJson();

// Validates a full replacement of the list, writes /devices.json on the card and applies it at
// once (no restart). A device with an empty or missing "key" keeps the key of the device with the
// same name; a new or renamed device must bring its 16-character key.
SaveResult save(const String& json);

LoadInfo info();
std::vector<Device> list();
bool find(const String& name, Device& out);
bool state(const String& name, State& out);   // false: no such device

// The cached on/off state of the device's switch data point: 1 on, 0 off, -1 unknown (never queried, offline,
// no such device, or the data point is not a boolean). Reads the cache; never touches the device.
int switchState(const String& name);

// The data points that carry the readings of `d` (its meters, standard numbers where unset, 0 = none skipped),
// as "18,19,20,17" for UPDATEDPS. "" if the device has no meters.
String meterDps(const Device& d);

// History of the readings: the last kHistoryCapacity status queries of a device (the monitor's polls and the user's
// queries), each with the time it finished. A failed query is a sample too (ok:false, no values), so gaps are visible.
// The values are the raw data points of the device's meters (power, current, voltage, energy: units as in the README,
// null when the device has none or the query failed) and the switch state. Times are uptime based, so they stay
// right when the clock is set later: "time" (UTC seconds) is given once the clock is valid, "age_s" always.
// Newest last. limit 0 = all. False: no such device.
constexpr size_t kHistoryCapacity = 80;
bool historyJson(const String& name, size_t limit, String& out);

// Devices that need fresh readings because an enabled scenario waits for their power (see Scenario): the monitor polls
// them with UPDATEDPS + status instead of the plain status query, which only returns what the device cached earlier
// (tens of seconds old). Set by the scenarios whenever they are loaded or saved; names only, so devices that are added,
// removed, edited or ignored later are handled by the monitor itself. Other devices never get an UPDATEDPS.
void setWatched(const std::vector<String>& names);
bool isWatched(const String& name);

// Does the power of `name` satisfy a rule for long enough? Read from the history (the answers of the status queries):
// the newest answers must all be on the right side of `watts` (below or above), the relay on, with no unanswered query
// and no gap between them, for `forS` seconds. A relay that was just switched on (or found on when the history began,
// e.g. after the power came back) gets `warmupS` seconds before the
// time counts (a heater is still ramping up, and the first readings are old). Anything that breaks the run (an
// unanswered query, the relay off, the power on the other side) restarts the count: no data never fires a rule.
struct PowerCheck {
  bool met = false;
  String reason;                    // met | waiting | warmup | not-met | relay-off | relay-unknown | unanswered | stale |
                                    // no-data | no-power-dp | ignored | no-device
  int32_t lastPower = INT32_MIN;    // the newest raw power reading (0.1 W), INT32_MIN if none
  uint32_t runS = 0;                // how long the rule has held (after the warm-up)
};
PowerCheck checkPower(const String& name, bool below, uint32_t watts, uint32_t forS, uint32_t warmupS);

// Registered as the Tuya observer: records the result of a user-started status query.
void onJobDone(const Tuya::Job& job);

// Starts the poller. attempts = tries per device and cycle (>= 1). Call once, after Tuya::begin().
void startMonitor(uint16_t intervalSec, uint16_t timeoutMs, uint8_t attempts);
uint16_t pollIntervalSec();

// How long the monitor's cycles over all the devices took: the last one and the longest since boot (ms, 0 = none yet).
// A cycle longer than the interval means every device is polled less often than configured (the power rules follow
// the real step, see PowerRule::pollStep).
void monitorCycle(uint32_t& lastMs, uint32_t& maxMs);

constexpr size_t kMaxDevices = 16;

// The limits of the device list and the history, for the API and the page: max, max_file_bytes, history_capacity,
// history_span_s (capacity x the current polling interval), poll_interval_s.
void limits(JsonObject out);

}  // namespace Devices
