#pragma once
//
// Scenarios: named lists of steps ("switch this socket on, wait, switch the next one on ...")
// that start on a trigger. There can be several, and several on one trigger; every running
// scenario is its own task, so they run side by side and know nothing about each other.
//
// Definitions live in /scenarios.json on the SD card (like /devices.json: no card, no
// scenarios) and are edited on the "Сценарії" page. A step names a device from /devices.json.
//
// Triggers:
//   power_on  starts `startup.scenario_delay_s` seconds after boot, but only after a real
//             power-up (reset reason POWERON). Any other start (software restart, panic,
//             watchdog, brown-out) does NOT start it, with one exception: a software restart
//             while the power-up's runs had not got Wi-Fi yet (the Wi-Fi watchdog in main.cpp,
//             or a restart from the settings page) starts them again, as a power-up.
//             Only enabled scenarios start on a trigger.
//   manual    never starts by itself.
//   power_below / power_above
//             starts when the power of one socket stays below / above a threshold for a time, with `when`:
//             { "device", "watts", "for_s", "warmup_s" }. Read from the history of the readings (Devices::checkPower):
//             every answer of the last for_s seconds on the right side, the relay on, no unanswered query and no gap;
//             a relay that was just switched on gets warmup_s first (default 60 s). It fires once when the condition
//             becomes true and is armed again only after the condition has been false at a look (the relay off, the
//             power on the other side, no data). While a scenario waits for such a rule, the monitor polls that device
//             with UPDATEDPS so its readings are fresh; no other device gets UPDATEDPS.
// Any scenario, enabled or not, can be started by hand (runNow) and stopped (stop).
//
// A step always sends its action (3 s timeout; the cached state is not trusted: it can be old or empty after a
// blackout, and "on" to a socket that is already on does no harm), then queries the state to see the new state,
// up to 3 attempts 2 s apart. After the last failed attempt the step is "failed" and the scenario goes on.
// A run started by the power-up is more patient, because the router and the sockets are still booting: it first waits
// for Wi-Fi (as long as it takes, then 10 s more for the sockets), and each step is retried for up to 90 s, 5 s apart.
// The pause after a step (its own, or the scenario's default) is kept only after a step that really
// switched something on. The commands go into the command lane of Tuya (with the buttons and the API),
// which runs beside the monitor's polls and does not wait for them.
//
// Caveat: ESP32 reports every reset through the EN pin (RST button, flashing, opening a serial
// port) as POWERON too, so on the debug base board those start power_on scenarios as well. On the
// deployed hardware (module powered directly) POWERON means power came back.
//
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_system.h>

#include <vector>

namespace Scenario {

// How many scenarios, and how big their file and PUT body may be, depends on the memory: with PSRAM the documents live
// there (24 x 24 steps with long names is ~58 KB of JSON; HTTP steps may carry up to 16 KB of body each); without it
// they come out of the ~100 KB of internal RAM.
constexpr size_t kMaxScenariosPsram = 24, kMaxScenariosNoPsram = 16;
constexpr size_t kMaxFileBytesPsram = 393216, kMaxFileBytesNoPsram = 49152;  // /scenarios.json (pretty if it fits, else compact)
constexpr size_t kMaxBodyBytesPsram = 262144, kMaxBodyBytesNoPsram = 49152;  // PUT /api/scenarios
size_t maxScenarios();
size_t maxFileBytes();
size_t maxBodyBytes();
constexpr size_t kMaxSteps = 24;
constexpr uint16_t kMaxPauseS = 3600;      // pause after a step
constexpr uint32_t kMaxWatts = 20000;      // power rule threshold
constexpr uint32_t kMinForS = 10, kMaxForS = 900;
constexpr uint32_t kMaxWarmupS = 3600;
constexpr size_t kHistoryFileBytes = 131072;  // the history of the runs: a file this big becomes the old one

// for_s a power rule may have now: kMaxForS, or less when the history of the readings (Devices::kHistoryCapacity
// answers at the current polling interval) is shorter.
uint32_t maxForS();

// All the limits above (and maxForS()) as JSON, for the API and the page.
void limits(JsonObject out);

// reasonName: text for the log, e.g. "SW". Call once, after Logger, Config, Devices and Tuya are up.
// Reads /scenarios.json and, after a power-up, starts the enabled power_on scenarios.
void begin(esp_reset_reason_t reason, const char* reasonName, uint16_t delaySec);

// The card was missing at start and is there now: reads /scenarios.json. Does nothing if the file was read already.
// Scenarios that start on power-up are NOT started by this (the power-up is over).
void sdAvailable();

// Which scenarios name a device that is not in `deviceNames`: one text per enabled step (and per power rule) such as
// "scenario 'power-restored', step 3: device 'heater1' is not in the list". For the warning after the device list is edited.
std::vector<String> missingDevices(const std::vector<String>& deviceNames);

enum class RunResult { Started, NotFound, AlreadyRunning };

// Starts the scenario now (enabled or not). While it is waiting for its trigger delay, the wait is cut short.
RunResult runNow(const String& name);

enum class StopResult { Stopping, NotFound, NotRunning };

// Asks a waiting or running scenario to stop: it ends after the step in progress (or at once while waiting).
StopResult stop(const String& name);

// JSON for GET /api/scenarios: the definitions (editor) with the last run of each (steps and their results).
String configJson();

// GET /api/scenarios/history: the ended runs, newest first, as {"runs":[...], "more":bool, "sd":bool}: at most `limit`, only
// those older than the run with id `before` (0: from the newest), only of scenario `name` ("": all). False without a card.
bool history(const String& name, size_t limit, uint32_t before, String& out);

// A short entry per scenario for GET /api/status.
void summary(JsonArray out);

// The engine's counters for GET /api/status: runs alive, the places in the command lane (max_in_flight, in_flight,
// max_in_flight_seen, slot_waits, slot_wait_max_ms, queue_full) and the attempts on devices the monitor counted offline
// (offline_attempts, offline_attempts_ok, offline_attempt_ms).
void engineStats(JsonObject out);

struct SaveResult {
  bool ok = false;
  bool invalid = false;   // the document was rejected (HTTP 400), see error
  bool noSd = false;      // no card (HTTP 503)
  size_t count = 0;       // scenarios saved
  String error;
};

// Validates a full replacement of the list, writes /scenarios.json and applies it at once (no restart).
// Scenarios that are no longer in the list are stopped. A run in progress keeps the steps it started with.
SaveResult save(const String& json);

}  // namespace Scenario
