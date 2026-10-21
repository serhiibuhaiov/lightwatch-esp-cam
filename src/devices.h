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
// poll_s (optional, 0..3600): the monitor queries the device once every poll_s seconds instead of every cycle
// (monitor.interval_s; absent, or not more than the interval: every cycle), whether it sends status pushes or not. Between
// two queries a device with a live kept connection gets a history sample of its cached state every cycle (the data points
// it sends on its own go into that state at once, see onPush); without a live kept connection it gets a sample only from a
// real query (the power rules take their gap from the real step). A kept connection that was lost is opened again by a
// query in the next cycle, poll_s or not. poll_s 0: no polling at all, only that query whenever the kept connection is
// (re)opened (or the device is offline); the rest comes from the pushes, so only with keep_connected (else it would never
// get any data). The old field "monitor" ("poll"/"push") is no longer read.
// meters (optional): which data point carries the power / current / voltage / energy reading, 0..255;
// 0 = the socket has none, absent = the usual Tuya socket numbers (19, 18, 20, 17). They label the readings
// on the page, fill the history, feed the power rules and say which data points UPDATEDPS re-reads;
// different socket models number them differently.
//
// State and monitor: a background task polls every device each monitor.interval_s (or its poll_s; up to
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

constexpr uint16_t kPollOnConnect = UINT16_MAX;  // poll_s 0: queried only when the kept connection is (re)opened

struct Device {
  String name;
  String ip;
  String key;
  Tuya::Version version = Tuya::Version::V34;
  uint16_t dp = 1;
  int16_t dpPower = -1, dpCurrent = -1, dpVoltage = -1, dpEnergy = -1;  // -1 = standard number, 0 = none
  bool ignore = false;  // not polled by the monitor (and no kept connection); scenario steps and buttons still switch it
  bool keepConnected = false;  // one connection kept open for every operation (see Tuya::Target::keep)
  // The monitor polls it with UPDATEDPS + status (fresh readings: a plain status query returns what the device cached
  // earlier), whether a power rule waits for it or not. false: plain status queries only (a socket that keeps its
  // readings fresh by itself, or that misbehaves on UPDATEDPS: the boiler switched itself off during one).
  bool updatedps = true;
  uint16_t pollS = 0;  // poll_s (see above); 0 = every monitor cycle, kPollOnConnect = poll_s 0
  // poll_s as the API and the file show it: 0 (only on connect), the seconds, or -1 for every cycle (null / absent).
  int32_t pollSField() const { return pollS == kPollOnConnect ? 0 : pollS ? pollS : -1; }

  Tuya::Target target() const {
    Tuya::Target t;
    t.ip = ip;
    t.key = key;
    t.version = version;
    t.keep = keepConnected && !ignore;  // the monitor closes the kept connections of ignored devices anyway
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
// offline needs no such confirmation: its first failure keeps it offline. One success makes it online again. A query that
// failed while the board itself had no Wi-Fi says nothing about the device: it changes nothing (as "busy").
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
  String dpsJson;           // the "dps" object of the last success, as JSON text ("" = none yet), with the pushes since merged in
  uint32_t lastPushMs = 0;  // millis() of the last status push applied (0 = none)
  uint32_t pushes = 0;      // status pushes applied
  // The switch changed to on / to off: the relaySeq() number of the last such change, 0 = none seen (the trigger "relay").
  // A change counts only when the device was online before it: not the first reading after a boot or an edit of the
  // device, nor the first answer after it was offline (the switch may have changed at any time while it was away).
  uint32_t onSeq = 0, offSeq = 0;
  // For the trigger "online": answered at least once (since boot, or since the device was edited), and the millis() of
  // the last change of `online` (or of the first query).
  bool everOnline = false;
  uint32_t changedMs = 0;
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
// The cached readings of `name` for the placeholders of an HTTP step: raw power, current, voltage, energy by its meters
// (INT32_MIN = none) and the switch (1 on, 0 off, -1 unknown). Only for a device that is online and not ignored: false,
// and everything none, otherwise. Reads the cache; never touches the device.
bool readings(const String& name, int32_t values[4], int& relay);
// The number of the last change of a switch on any device (0 = none since boot): every change takes the next number, so a
// change after a look at this number has a greater one (State::onSeq / offSeq), even on a device whose state was reset.
uint32_t relaySeq();

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

// Energy since the board started (lib/energy_meter), counted two ways side by side: `integrated` (power x time, from the
// power reading of every answer and push; a switch that is off is 0 W) and `reported` (the sum of the socket's energy data
// point, meters.energy: a pushed value always counts, one read by a query only when it changed, the first is the base).
// Not kept across a restart. A device without a power data point (meters.power 0) has no integrated energy, one without
// an energy data point no reported energy; an ignored one counts nothing. Kept across an edit of the device (the time it was lost is not counted, the
// reported energy takes a new base); a removed device loses it. The values the socket's `energy` reading shows elsewhere
// (state, history, {{x.energy}}) are untouched.
struct Energy {
  bool integratedKnown = false;   // a power was held at least once
  double integratedWh = 0;
  uint32_t sinceS = 0;            // since the first answer or push after the board started
  uint32_t coveredS = 0;          // how much of it was counted (the rest: the device was lost, or had no power reading)
  int32_t heldPowerRaw = INT32_MIN;  // the power held now, 0.1 W (INT32_MIN: none: not answering, lost)
  bool reportedKnown = false;     // the energy data point was seen at least once
  uint64_t reportedWh = 0;
  uint32_t reports = 0;           // values of the energy data point seen (pushed or read)
  uint32_t reportsCounted = 0;    // ... and added
  int32_t lastReportRaw = INT32_MIN;
};
bool energy(const String& name, Energy& out);  // false: no such device
// As JSON: integrated_wh (0.1 Wh) and reported_wh (null: not known), since_s, covered_s, power_w (held now, null: none),
// reports, reports_counted, last_report (raw, null: none).
void writeEnergy(const Energy& e, JsonObject o);

// Its history, in PSRAM only (none without it): a point every kEnergyPointS seconds (both sums and the power held then)
// for the last kEnergyPoints of them, and the last kEnergyReports values of the energy data point (pushed or read, and
// whether they were counted). Newest last; limit 0 = all points. False: no such device.
constexpr size_t kEnergyPoints = 1440;  // 24 h
constexpr uint32_t kEnergyPointS = 60;
constexpr size_t kEnergyReports = 128;
bool energyJson(const String& name, size_t limit, String& out);
// Adds the points that are due; the monitor task calls it every cycle.
void energyTick();

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

// Registered as the Tuya push observer: merges the data points a device sent on its own into its cached state (a push
// carries only those that changed) and logs a change of the switch. Dropped for a device that is ignored or was edited.
void onPush(const String& device, const Tuya::Target& target, const String& json);

// Starts the poller. attempts = tries per device and cycle (>= 1). Call once, after Tuya::begin().
void startMonitor(uint16_t intervalSec, uint16_t timeoutMs, uint8_t attempts);
uint16_t pollIntervalSec();

// How long the monitor's cycles over all the devices took: the last one and the longest since boot (ms, 0 = none yet).
// A cycle longer than the interval means every device is polled less often than configured (the power rules follow
// the real step, see PowerRule::pollStep).
void monitorCycle(uint32_t& lastMs, uint32_t& maxMs);

constexpr size_t kMaxDevices = 16;
constexpr uint16_t kMaxPollS = 3600;  // the largest poll_s

// The limits of the device list and the history, for the API and the page: max, max_file_bytes, history_capacity,
// history_span_s (capacity x the current polling interval), poll_interval_s, poll_s_max, energy (points, point_s, reports,
// psram: whether there is a history at all).
void limits(JsonObject out);

}  // namespace Devices
