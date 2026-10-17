#include "scenario.h"

#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <WiFi.h>

#include <map>
#include <memory>
#include <vector>

#include "devices.h"
#include "logger.h"
#include "storage.h"
#include "tuya.h"

namespace Scenario {
namespace {

const char* TAG = "scenario";
const char* kPath = "/scenarios.json";
const char* kTmp = "/scenarios.tmp";
const char* kBak = "/scenarios.bak";
const char* kBakTmp = "/scenarios.bak.tmp";
constexpr size_t kMaxFileBytes = 32768;  // kMaxScenarios x kMaxSteps steps

constexpr uint8_t kAttempts = 3;          // tries per step
constexpr uint32_t kCommandTimeoutMs = 3000;
constexpr uint32_t kRetryGapMs = 2000;
constexpr uint32_t kQueueWaitMs = 120000; // how long a command may wait in the Tuya queue before the attempt is given up
constexpr uint16_t kMaxPauseS = 3600;

// A run started by the power-up trigger cannot count on the network: after a blackout the router (and the sockets) take
// a minute or two to come back. So it waits for Wi-Fi first (as long as it takes: no command gets through without it)
// and then keeps trying each step for a longer window.
constexpr uint32_t kNetSettleMs = 10000;              // after Wi-Fi came up: let the sockets join the network
constexpr uint32_t kPatientWindowMs = 90000;          // retry window of one step in such a run
constexpr uint32_t kPatientGapMs = 5000;

const char* kTriggerPowerOn = "power_on";
const char* kTriggerManual = "manual";
const char* kTriggerPowerBelow = "power_below";
const char* kTriggerPowerAbove = "power_above";

// A power trigger: `when` = { device, watts, for_s, warmup_s }.
constexpr uint32_t kMaxWatts = 20000;
constexpr uint32_t kMinForS = 10, kMaxForS = 900;
constexpr uint32_t kDefaultWarmupS = 60, kMaxWarmupS = 3600;
constexpr uint32_t kWatchTickMs = 3000;  // how often the conditions are looked at

enum class State : uint8_t { Idle, Skipped, Waiting, Running, Done, Stopped };

const char* stateName(State s) {
  switch (s) {
    case State::Skipped: return "skipped";
    case State::Waiting: return "waiting";
    case State::Running: return "running";
    case State::Done: return "done";
    case State::Stopped: return "stopped";
    default: return "idle";
  }
}

struct Step {
  String device;
  bool on = true;            // the action: switch the device on (default) or off
  bool enabled = true;
  int32_t pauseAfterS = -1;  // -1: the scenario's default
};

struct Def {
  String name;
  bool enabled = true;
  String trigger = kTriggerPowerOn;
  uint16_t defaultPauseS = 30;
  std::vector<Step> steps;
  // power_below / power_above only:
  String whenDevice;
  uint32_t watts = 0;
  uint32_t forS = 0;
  uint32_t warmupS = kDefaultWarmupS;

  bool isPower() const { return trigger == kTriggerPowerBelow || trigger == kTriggerPowerAbove; }
};

// pending | disabled | running | ok | skipped | failed | stopped
struct StepRun {
  String device;
  String state = "pending";
  uint8_t attempts = 0;
  String detail;
};

struct Runtime {
  State state = State::Idle;
  String trigger;            // what started this run: "power-on" | "manual"
  String skipReason;
  uint32_t startAtMs = 0;    // millis() at which a waiting run starts
  String waitReason;         // while waiting past startAtMs: what for ("wifi")
  int current = -1;         // index of the step in progress
  std::vector<StepRun> steps;
  bool alive = false;        // a task exists for this run
  bool stop = false;         // asked to stop
  bool holdsPowerUp = false; // started by the power-up and has not got past the wait for the network yet
};

// A power-up whose power_on runs are still waiting for the network is carried over a software restart: the Wi-Fi watchdog
// in main.cpp restarts the board (reset reason SW) when Wi-Fi stays away, and that must not lose the power-up. RTC memory
// keeps its value over a software restart and loses it when the power goes.
constexpr uint32_t kPowerUpPending = 0x50574F4E;  // "PWON"
RTC_NOINIT_ATTR uint32_t gPowerUpPending;

SemaphoreHandle_t gMux = nullptr;      // guards everything below
SemaphoreHandle_t gSaveMux = nullptr;  // serialises save(); held across the SD write
std::vector<Def> gDefs;
std::map<String, Runtime> gRun;        // by scenario name; the std::map nodes stay in place
struct {
  bool sd = false;
  bool fileFound = false;
  String error;
} gInfo;

struct Lock {
  Lock() { xSemaphoreTake(gMux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(gMux); }
};

struct SaveLock {
  SaveLock() { xSemaphoreTake(gSaveMux, portMAX_DELAY); }
  ~SaveLock() { xSemaphoreGive(gSaveMux); }
};

bool validName(const String& s) {
  if (s.isEmpty() || s.length() > 32) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool validTrigger(const String& t) {
  return t == kTriggerPowerOn || t == kTriggerManual || t == kTriggerPowerBelow || t == kTriggerPowerAbove;
}

// ---- the file ----------------------------------------------------------------------------

bool readBool(JsonVariantConst v, const String& where, bool& out, String& err) {
  if (v.isNull()) return true;  // absent: keep the default
  if (!v.is<bool>()) {
    err = where + " must be true or false";
    return false;
  }
  out = v.as<bool>();
  return true;
}

// Parses the whole document; on success `out` holds the list, on failure `err` says why.
bool parse(const String& raw, std::vector<Def>& out, String& err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, raw);
  if (e) {
    err = String("invalid JSON: ") + e.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    err = "root must be an object";
    return false;
  }
  JsonVariantConst arr = doc["scenarios"];
  if (!arr.is<JsonArrayConst>()) {
    err = "\"scenarios\" must be an array";
    return false;
  }
  if (arr.size() > kMaxScenarios) {
    err = String("more than ") + String(static_cast<unsigned>(kMaxScenarios)) + " scenarios";
    return false;
  }

  std::vector<Def> list;
  size_t si = 0;
  for (JsonVariantConst item : arr.as<JsonArrayConst>()) {
    const String where = String("scenarios[") + String(static_cast<unsigned>(si++)) + "]";
    if (!item.is<JsonObjectConst>()) {
      err = where + " must be an object";
      return false;
    }
    Def d;
    if (!item["name"].is<const char*>()) {
      err = where + ".name must be a string";
      return false;
    }
    d.name = item["name"].as<const char*>();
    if (!validName(d.name)) {
      err = where + ".name must be 1..32 chars of a-z 0-9 _ -";
      return false;
    }
    for (const Def& other : list) {
      if (other.name == d.name) {
        err = where + ".name '" + d.name + "' is used twice";
        return false;
      }
    }
    if (!readBool(item["enabled"], where + ".enabled", d.enabled, err)) return false;
    if (!item["trigger"].is<const char*>() || !validTrigger(item["trigger"].as<const char*>())) {
      err = where + ".trigger must be \"power_on\", \"manual\", \"power_below\" or \"power_above\"";
      return false;
    }
    d.trigger = item["trigger"].as<const char*>();
    if (d.isPower()) {
      JsonVariantConst w = item["when"];
      if (!w.is<JsonObjectConst>()) {
        err = where + ".when must be an object {device, watts, for_s} for the trigger " + d.trigger;
        return false;
      }
      if (!w["device"].is<const char*>() || !validName(w["device"].as<const char*>())) {
        err = where + ".when.device must be a device name (1..32 chars of a-z 0-9 _ -)";
        return false;
      }
      d.whenDevice = w["device"].as<const char*>();
      if (!w["watts"].is<int>() || w["watts"].as<int>() < 0 || w["watts"].as<int>() > static_cast<int>(kMaxWatts)) {
        err = where + ".when.watts must be an integer 0.." + String(static_cast<unsigned>(kMaxWatts)) + " (watts)";
        return false;
      }
      d.watts = w["watts"].as<int>();
      if (!w["for_s"].is<int>() || w["for_s"].as<int>() < static_cast<int>(kMinForS) || w["for_s"].as<int>() > static_cast<int>(kMaxForS)) {
        err = where + ".when.for_s must be an integer " + String(static_cast<unsigned>(kMinForS)) + ".." + String(static_cast<unsigned>(kMaxForS)) + " (seconds)";
        return false;
      }
      d.forS = w["for_s"].as<int>();
      if (!w["warmup_s"].isNull()) {
        if (!w["warmup_s"].is<int>() || w["warmup_s"].as<int>() < 0 || w["warmup_s"].as<int>() > static_cast<int>(kMaxWarmupS)) {
          err = where + ".when.warmup_s must be an integer 0.." + String(static_cast<unsigned>(kMaxWarmupS)) + " (seconds)";
          return false;
        }
        d.warmupS = w["warmup_s"].as<int>();
      }
    }

    if (!item["default_pause_s"].isNull()) {
      if (!item["default_pause_s"].is<int>() || item["default_pause_s"].as<int>() < 0 || item["default_pause_s"].as<int>() > kMaxPauseS) {
        err = where + ".default_pause_s must be an integer 0.." + String(static_cast<unsigned>(kMaxPauseS));
        return false;
      }
      d.defaultPauseS = item["default_pause_s"].as<int>();
    }

    JsonVariantConst steps = item["steps"];
    if (!steps.is<JsonArrayConst>()) {
      err = where + ".steps must be an array";
      return false;
    }
    if (steps.size() > kMaxSteps) {
      err = where + " has more than " + String(static_cast<unsigned>(kMaxSteps)) + " steps";
      return false;
    }
    size_t pi = 0;
    for (JsonVariantConst sv : steps.as<JsonArrayConst>()) {
      const String at = where + ".steps[" + String(static_cast<unsigned>(pi++)) + "]";
      if (!sv.is<JsonObjectConst>()) {
        err = at + " must be an object";
        return false;
      }
      Step st;
      if (!sv["device"].is<const char*>() || !validName(sv["device"].as<const char*>())) {
        err = at + ".device must be a device name (1..32 chars of a-z 0-9 _ -)";
        return false;
      }
      st.device = sv["device"].as<const char*>();
      if (!sv["action"].isNull()) {
        const String a = sv["action"].is<const char*>() ? String(sv["action"].as<const char*>()) : String("");
        if (a != "on" && a != "off") {
          err = at + ".action must be \"on\" or \"off\"";
          return false;
        }
        st.on = a == "on";
      }
      if (!readBool(sv["enabled"], at + ".enabled", st.enabled, err)) return false;
      if (!sv["pause_after_s"].isNull()) {
        if (!sv["pause_after_s"].is<int>() || sv["pause_after_s"].as<int>() < 0 || sv["pause_after_s"].as<int>() > kMaxPauseS) {
          err = at + ".pause_after_s must be an integer 0.." + String(static_cast<unsigned>(kMaxPauseS)) + " or absent (the default)";
          return false;
        }
        st.pauseAfterS = sv["pause_after_s"].as<int>();
      }
      d.steps.push_back(st);
    }
    list.push_back(d);
  }
  out = list;
  return true;
}

String fileText(const std::vector<Def>& list, bool pretty) {
  JsonDocument doc;
  doc["version"] = 1;
  JsonArray arr = doc["scenarios"].to<JsonArray>();
  for (const Def& d : list) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = d.name;
    o["enabled"] = d.enabled;
    o["trigger"] = d.trigger;
    if (d.isPower()) {
      JsonObject w = o["when"].to<JsonObject>();
      w["device"] = d.whenDevice;
      w["watts"] = d.watts;
      w["for_s"] = d.forS;
      w["warmup_s"] = d.warmupS;
    }
    o["default_pause_s"] = d.defaultPauseS;
    JsonArray steps = o["steps"].to<JsonArray>();
    for (const Step& s : d.steps) {
      JsonObject so = steps.add<JsonObject>();
      so["device"] = s.device;
      so["action"] = s.on ? "on" : "off";
      so["enabled"] = s.enabled;
      if (s.pauseAfterS >= 0) so["pause_after_s"] = s.pauseAfterS;
    }
  }
  String out;
  if (pretty) {
    serializeJsonPretty(doc, out);
  } else {
    serializeJson(doc, out);
  }
  out += "\n";
  return out;
}

void load() {
  std::vector<Def> list;
  String error;
  bool fileFound = false;
  bool replace = true;
  const bool sd = Storage::sdMounted();
  // -1 also means "could not open": a card that has just come up can fail the first look (see Config::load).
  long size = sd ? Storage::sdSize(kPath) : -1;
  for (int i = 0; i < 2 && sd && size < 0; ++i) {
    delay(200);
    size = Storage::sdSize(kPath);
  }
  if (!sd) {
    error = "no SD card: scenarios are stored only on the card";
    LOGW(TAG, "%s", error.c_str());
  } else if (size < 0) {
    // A save removes the old file before renaming the new one: a power cut in between leaves only the
    // complete temporary file. Never replace that with an empty template.
    String tmp, tmpErr;
    std::vector<Def> tmpList;
    if (Storage::sdReadAll(kTmp, tmp, kMaxFileBytes) && parse(tmp, tmpList, tmpErr) && Storage::sdReplace(kTmp, kPath)) {
      list = tmpList;
      fileFound = true;
      LOGW(TAG, "promoted %s to %s after an interrupted save", kTmp, kPath);
    } else {
      // sdCreate never overwrites: if the file is there after all (the card hid it), it stays.
      static const char kTemplate[] = "{\n  \"version\": 1,\n  \"scenarios\": []\n}\n";
      if (Storage::sdCreate(kPath, kTmp, String(kTemplate))) {
        fileFound = true;
        LOGW(TAG, "no %s, wrote an empty template: add scenarios on the Scenarios page", kPath);
      } else {
        error = String("cannot open ") + kPath + " and could not create it (card error?)";
        LOGE(TAG, "%s", error.c_str());
        replace = false;
      }
    }
  } else {
    fileFound = true;
    String raw, err;
    if (!Storage::sdReadAll(kPath, raw, kMaxFileBytes)) {
      error = String("cannot read ") + kPath + " (too large or card error)";
      LOGE(TAG, "%s", error.c_str());
      replace = false;
    } else if (!parse(raw, list, err)) {
      error = String(kPath) + ": " + err;
      LOGE(TAG, "%s (no scenarios are active)", error.c_str());
    }
  }
  Lock lock;
  if (replace) gDefs = list;
  gInfo.sd = sd;
  gInfo.fileFound = fileFound;
  gInfo.error = error;
  if (error.isEmpty()) LOGI(TAG, "%u scenario(s) loaded from %s", static_cast<unsigned>(gDefs.size()), kPath);
}

// ---- running -------------------------------------------------------------------------------

void setStep(const String& name, size_t idx, const char* state, uint8_t attempts, const String& detail) {
  Lock lock;
  auto it = gRun.find(name);
  if (it == gRun.end() || idx >= it->second.steps.size()) return;
  StepRun& s = it->second.steps[idx];
  s.state = state;
  s.attempts = attempts;
  s.detail = detail;
}

bool stopRequested(const String& name) {
  Lock lock;
  auto it = gRun.find(name);
  return it == gRun.end() || it->second.stop;
}

// Sleeps `ms`; false if the scenario was asked to stop meanwhile.
bool sleepOrStop(const String& name, uint32_t ms) {
  const uint32_t until = millis() + ms;
  while (static_cast<int32_t>(until - millis()) > 0) {
    if (stopRequested(name)) return false;
    const int32_t left = static_cast<int32_t>(until - millis());
    vTaskDelay(pdMS_TO_TICKS(left > 200 ? 200 : left));
  }
  return !stopRequested(name);
}

// Queues one command in the command lane (beside the monitor's polls, see tuya.h) and waits for it.
// nullptr: it could not be queued or did not finish in time.
std::shared_ptr<Tuya::Job> runJob(Tuya::Op op, const Devices::Device& d) {
  std::shared_ptr<Tuya::Job> job;
  for (int i = 0; i < 40 && !job; ++i) {
    job = Tuya::submit(op, d.name, d.target(), d.dp, kCommandTimeoutMs, false);
    if (!job) vTaskDelay(pdMS_TO_TICKS(500));  // the queue is full: other scenarios or the user are using it
  }
  if (!job) return nullptr;
  const uint32_t until = millis() + kQueueWaitMs;
  while (!job->done.load(std::memory_order_acquire)) {
    if (static_cast<int32_t>(until - millis()) <= 0) {
      // Giving up: a command that is still waiting in the queue is withdrawn, so that it does not reach the socket
      // minutes later, after the scenario has moved on (or was stopped). One that is already running cannot be recalled.
      job->cancelled.store(true);
      return nullptr;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return job;
}

// One attempt: the command (on or off), then a status query that must show the new state. `why` says what went wrong.
bool attemptSwitch(const Devices::Device& d, bool on, String& why) {
  std::shared_ptr<Tuya::Job> cmd = runJob(on ? Tuya::Op::On : Tuya::Op::Off, d);
  if (!cmd) {
    why = "the command did not get through the device queue in time";
    return false;
  }
  if (!cmd->result.ok) {
    why = cmd->result.error + ": " + cmd->result.detail;
    return false;
  }
  // The query's own answer updates the cached state (the observer runs before the job is done).
  std::shared_ptr<Tuya::Job> q = runJob(Tuya::Op::Query, d);
  if (!q) {
    why = "the command was accepted, but the status query did not get through in time";
    return false;
  }
  if (!q->result.ok) {
    why = "the command was accepted, but the status query failed: " + q->result.error;
    return false;
  }
  if (Devices::switchState(d.name) != (on ? 1 : 0)) {
    why = String("the command was accepted, but the status does not show it ") + (on ? "on" : "off");
    return false;
  }
  return true;
}

enum class Outcome { Switched, Skipped, Failed, Stopped };

// `patient`: keep trying for kPatientWindowMs (a run started by the power-up) instead of kAttempts times.
Outcome runStep(const String& scn, size_t idx, const Step& step, bool patient) {
  const char* verb = step.on ? "on" : "off";
  Devices::Device d;
  if (!Devices::find(step.device, d)) {
    setStep(scn, idx, "failed", 0, "no such device in /devices.json");
    LOGW(TAG, "[%s] step %u: no device '%s' in /devices.json", scn.c_str(), static_cast<unsigned>(idx + 1), step.device.c_str());
    return Outcome::Failed;
  }
  if (d.ignore) {
    setStep(scn, idx, "skipped", 0, "the device is ignored (see the device settings)");
    LOGI(TAG, "[%s] step %u: %s is ignored, skipped", scn.c_str(), static_cast<unsigned>(idx + 1), d.name.c_str());
    return Outcome::Skipped;
  }
  String last;
  const uint32_t windowEnd = millis() + kPatientWindowMs;
  const uint32_t gapMs = patient ? kPatientGapMs : kRetryGapMs;
  uint8_t attempt = 0;
  for (;;) {
    ++attempt;
    if (stopRequested(scn)) {
      setStep(scn, idx, "stopped", attempt - 1, "stopped");
      return Outcome::Stopped;
    }
    setStep(scn, idx, "running", attempt, attempt > 1 ? "previous attempt: " + last : String());
    String why;
    if (attemptSwitch(d, step.on, why)) {
      setStep(scn, idx, "ok", attempt,
              String("switched ") + verb + (attempt > 1 ? " after " + String(static_cast<unsigned>(attempt)) + " attempts" : String()));
      LOGI(TAG, "[%s] step %u: %s is %s (attempt %u)", scn.c_str(), static_cast<unsigned>(idx + 1), d.name.c_str(), verb,
           static_cast<unsigned>(attempt));
      return Outcome::Switched;
    }
    last = why;
    LOGW(TAG, "[%s] step %u: %s %s attempt %u failed: %s", scn.c_str(), static_cast<unsigned>(idx + 1), d.name.c_str(), verb,
         static_cast<unsigned>(attempt), why.c_str());
    const bool more = patient ? (static_cast<int32_t>(windowEnd - millis()) > static_cast<int32_t>(gapMs) && attempt < 255) : attempt < kAttempts;
    if (!more) break;
    if (!sleepOrStop(scn, gapMs)) {
      setStep(scn, idx, "stopped", attempt, "stopped");
      return Outcome::Stopped;
    }
  }
  setStep(scn, idx, "failed", attempt, last);
  LOGW(TAG, "[%s] step %u: %s FAILED after %u attempts, going on", scn.c_str(), static_cast<unsigned>(idx + 1), d.name.c_str(),
       static_cast<unsigned>(attempt));
  return Outcome::Failed;
}

void finish(const String& name, State state, const char* skipReason = "") {
  Lock lock;
  auto it = gRun.find(name);
  if (it == gRun.end()) return;
  it->second.state = state;
  it->second.current = -1;
  it->second.skipReason = skipReason;
}

bool triggerIs(const String& name, const char* trigger) {
  Lock lock;
  auto it = gRun.find(name);
  return it != gRun.end() && it->second.trigger == trigger;
}

void setWaitReason(const String& name, const char* reason) {
  Lock lock;
  auto it = gRun.find(name);
  if (it != gRun.end()) it->second.waitReason = reason;
}

// This run no longer waits for the network (it got it, was stopped or was started by hand): once no run of the power-up
// waits any more, the power-up has been served and a software restart must not repeat it.
void powerUpServed(const String& name) {
  Lock lock;
  auto it = gRun.find(name);
  if (it == gRun.end() || !it->second.holdsPowerUp) return;
  it->second.holdsPowerUp = false;
  for (const auto& kv : gRun) {
    if (kv.second.holdsPowerUp) return;
  }
  gPowerUpPending = 0;
}

// A run started by the power-up: waits until Wi-Fi is up, then kNetSettleMs for the sockets. There is no limit: if
// Wi-Fi stays away, the watchdog in main.cpp restarts the board and the power-up is carried over (gPowerUpPending).
// A run that was waiting and gets started by hand meanwhile (runNow changes its trigger) does not wait any more.
// False: stopped meanwhile.
bool waitForNetwork(const String& name) {
  if (WiFi.status() == WL_CONNECTED) return true;
  LOGI(TAG, "[%s] waiting for Wi-Fi", name.c_str());
  setWaitReason(name, "wifi");
  bool up = false;
  uint32_t settleUntil = 0;
  bool ok = true;
  for (;;) {
    if (stopRequested(name)) {
      ok = false;
      break;
    }
    if (!triggerIs(name, "power-on")) break;  // started by hand
    const uint32_t now = millis();
    if (!up) {
      if (WiFi.status() == WL_CONNECTED) {
        up = true;
        settleUntil = now + kNetSettleMs;
        LOGI(TAG, "[%s] Wi-Fi is up, %u s for the sockets to join", name.c_str(), static_cast<unsigned>(kNetSettleMs / 1000));
      }
    } else if (static_cast<int32_t>(settleUntil - now) <= 0) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  setWaitReason(name, "");
  return ok;
}

void runScenario(const String& name) {
  // Wait for the start time (short steps: runNow() and stop() act at once).
  for (;;) {
    uint32_t startAt;
    bool stop;
    {
      Lock lock;
      Runtime& r = gRun[name];
      startAt = r.startAtMs;
      stop = r.stop;
    }
    if (stop) {
      powerUpServed(name);
      finish(name, State::Stopped);
      LOGI(TAG, "[%s] stopped while waiting", name.c_str());
      return;
    }
    const int32_t left = static_cast<int32_t>(startAt - millis());
    if (left <= 0) break;
    vTaskDelay(pdMS_TO_TICKS(left > 200 ? 200 : left));
  }

  const bool netOk = !triggerIs(name, "power-on") || waitForNetwork(name);
  powerUpServed(name);
  if (!netOk) {
    finish(name, State::Stopped);
    LOGI(TAG, "[%s] stopped while waiting for Wi-Fi", name.c_str());
    return;
  }

  // The steps as they are now (an edit made while the run was waiting counts).
  Def def;
  bool found = false;
  {
    Lock lock;
    for (const Def& d : gDefs) {
      if (d.name == name) {
        def = d;
        found = true;
      }
    }
    if (found) {
      Runtime& r = gRun[name];
      r.state = State::Running;
      r.steps.assign(def.steps.size(), StepRun());
      for (size_t i = 0; i < def.steps.size(); ++i) {
        r.steps[i].device = def.steps[i].device;
        if (!def.steps[i].enabled) r.steps[i].state = "disabled";
      }
    }
  }
  if (!found) {
    finish(name, State::Skipped, "the scenario was removed before it started");
    LOGW(TAG, "[%s] removed before it started", name.c_str());
    return;
  }

  String how;
  {
    Lock lock;
    how = gRun[name].trigger;
  }
  const bool patient = how == "power-on";
  LOGI(TAG, "[%s] started (%s), %u step(s)", name.c_str(), how.c_str(), static_cast<unsigned>(def.steps.size()));

  unsigned switched = 0, skipped = 0, failed = 0;
  bool stopped = false;
  uint32_t pauseS = 0;
  size_t pauseFrom = 0;
  for (size_t i = 0; i < def.steps.size() && !stopped; ++i) {
    const Step& st = def.steps[i];
    if (!st.enabled) continue;
    if (pauseS > 0) {
      LOGI(TAG, "[%s] pause %u s before step %u", name.c_str(), static_cast<unsigned>(pauseS), static_cast<unsigned>(i + 1));
      {
        Lock lock;
        auto it = gRun.find(name);
        if (it != gRun.end() && pauseFrom < it->second.steps.size()) {
          it->second.steps[pauseFrom].detail += "; pause " + String(static_cast<unsigned>(pauseS)) + " s before the next step";
          it->second.current = -1;
        }
      }
      if (!sleepOrStop(name, pauseS * 1000UL)) {
        stopped = true;
        break;
      }
      pauseS = 0;
    }
    {
      Lock lock;
      auto it = gRun.find(name);
      if (it != gRun.end()) it->second.current = static_cast<int>(i);
    }
    switch (runStep(name, i, st, patient)) {
      case Outcome::Switched:
        ++switched;
        pauseS = st.pauseAfterS >= 0 ? static_cast<uint32_t>(st.pauseAfterS) : def.defaultPauseS;  // only after a step that switched something
        pauseFrom = i;
        break;
      case Outcome::Skipped: ++skipped; break;
      case Outcome::Failed: ++failed; break;
      case Outcome::Stopped: stopped = true; break;
    }
  }
  finish(name, stopped ? State::Stopped : State::Done);
  LOGI(TAG, "[%s] %s: %u switched, %u skipped, %u failed", name.c_str(), stopped ? "stopped" : "finished", switched, skipped, failed);
}

void taskMain(void* arg) {
  String* p = static_cast<String*>(arg);
  const String name = *p;
  delete p;
  runScenario(name);
  {
    Lock lock;
    auto it = gRun.find(name);
    if (it != gRun.end()) it->second.alive = false;
  }
  vTaskDelete(nullptr);
}

// Caller holds the lock; no task is alive for `name`.
bool startRun(const String& name, uint32_t startAtMs, const char* trigger) {
  Runtime& r = gRun[name];
  r = Runtime();
  r.state = State::Waiting;
  r.trigger = trigger;
  r.startAtMs = startAtMs;
  r.alive = true;
  if (xTaskCreate(taskMain, "scenario", 6144, new String(name), 1, nullptr) != pdPASS) {
    r.alive = false;
    r.state = State::Idle;
    LOGE(TAG, "[%s] could not create the task", name.c_str());
    return false;
  }
  return true;
}

// ---- power triggers ----------------------------------------------------------------------------

// The state of a power rule, for the page and for "fire once": it fires when the condition becomes true and is armed
// again only after the condition has been false at a look.
struct Watch {
  bool armed = true;
  bool met = false;
  String reason = "no-data";
  uint32_t runS = 0;
  int32_t power = INT32_MIN;  // raw, 0.1 W
};
std::map<String, Watch> gWatch;  // by scenario name

RunResult runWith(const String& name, const char* trigger);  // below

// Tells the monitor which devices need UPDATEDPS: the ones an enabled power rule waits for. Caller holds no lock.
void refreshWatched() {
  std::vector<String> names;
  {
    Lock lock;
    for (const Def& d : gDefs) {
      if (!d.enabled || !d.isPower()) continue;
      bool have = false;
      for (const String& n : names) have = have || n == d.whenDevice;
      if (!have) names.push_back(d.whenDevice);
    }
  }
  Devices::setWatched(names);
}

void watchTick() {
  std::vector<Def> rules;
  {
    Lock lock;
    for (const Def& d : gDefs) {
      if (d.enabled && d.isPower()) rules.push_back(d);
    }
    for (auto it = gWatch.begin(); it != gWatch.end();) {  // forget the rules that are gone or switched off
      bool present = false;
      for (const Def& d : rules) present = present || d.name == it->first;
      it = present ? std::next(it) : gWatch.erase(it);
    }
  }
  for (const Def& d : rules) {
    const bool below = d.trigger == kTriggerPowerBelow;
    Devices::PowerCheck pc = Devices::checkPower(d.whenDevice, below, d.watts, d.forS, d.warmupS);
    if (d.forS > (Devices::kHistoryCapacity - 2) * static_cast<uint32_t>(Devices::pollIntervalSec())) {
      pc.met = false;  // longer than the history holds at the current polling interval: it could never be shown
      pc.reason = "too-long";
    }
    bool fire = false;
    {
      Lock lock;
      Watch& w = gWatch[d.name];
      w.met = pc.met;
      w.reason = pc.reason;
      w.runS = pc.runS;
      w.power = pc.lastPower;
      if (pc.met && w.armed) {
        fire = true;
      } else if (!pc.met) {
        w.armed = true;
      }
    }
    if (!fire) continue;
    // Still running (an earlier run is not over): not started, the rule stays armed and is looked at again next time.
    if (runWith(d.name, below ? "power-below" : "power-above") == RunResult::Started) {
      {
        Lock lock;
        gWatch[d.name].armed = false;
      }
      LOGI(TAG, "[%s] %s: %s held for %u s (>= %u s): started", d.name.c_str(), d.whenDevice.c_str(),
           below ? "power below the threshold" : "power above the threshold", static_cast<unsigned>(pc.runS), static_cast<unsigned>(d.forS));
    }
  }
}

void watcherTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kWatchTickMs));
    watchTick();
  }
}

// ---- JSON ------------------------------------------------------------------------------------

unsigned doneSteps(const Runtime& r) {
  unsigned n = 0;
  for (const StepRun& s : r.steps) {
    if (s.state == "ok" || s.state == "skipped" || s.state == "failed" || s.state == "stopped") ++n;
  }
  return n;
}

unsigned totalSteps(const Runtime& r) {
  unsigned n = 0;
  for (const StepRun& s : r.steps) {
    if (s.state != "disabled") ++n;
  }
  return n;
}

uint32_t startsIn(const Runtime& r) {
  const int32_t left = static_cast<int32_t>(r.startAtMs - millis());
  return left > 0 ? static_cast<uint32_t>(left) / 1000UL + 1 : 0;
}

void describe(JsonObject o, const Def& d, const Runtime* r, bool full) {
  o["name"] = d.name;
  o["enabled"] = d.enabled;
  o["trigger"] = d.trigger;
  const State st = r ? r->state : State::Idle;
  o["state"] = stateName(st);
  if (r) {
    if (st == State::Waiting) {
      o["starts_in_s"] = startsIn(*r);
      if (!r->waitReason.isEmpty()) o["wait_reason"] = r->waitReason;
    }
    if (st == State::Skipped) o["skip_reason"] = r->skipReason;
    if (st == State::Waiting || st == State::Running || st == State::Done || st == State::Stopped) o["run_trigger"] = r->trigger;
    if (!r->steps.empty()) {
      o["progress_done"] = doneSteps(*r);
      o["progress_total"] = totalSteps(*r);
    }
  }
  if (!full) return;
  o["default_pause_s"] = d.defaultPauseS;
  if (d.isPower()) {
    JsonObject w = o["when"].to<JsonObject>();
    w["device"] = d.whenDevice;
    w["watts"] = d.watts;
    w["for_s"] = d.forS;
    w["warmup_s"] = d.warmupS;
    auto it = gWatch.find(d.name);  // the caller holds the lock
    if (it != gWatch.end()) {
      JsonObject ws = o["watch"].to<JsonObject>();
      ws["armed"] = it->second.armed;
      ws["met"] = it->second.met;
      ws["reason"] = it->second.reason;
      ws["run_s"] = it->second.runS;
      if (it->second.power == INT32_MIN) {
        ws["power_w"] = nullptr;
      } else {
        ws["power_w"] = it->second.power / 10.0;
      }
    }
  }
  JsonArray steps = o["steps"].to<JsonArray>();
  for (const Step& s : d.steps) {
    JsonObject so = steps.add<JsonObject>();
    so["device"] = s.device;
    so["action"] = s.on ? "on" : "off";
    so["enabled"] = s.enabled;
    if (s.pauseAfterS >= 0) {
      so["pause_after_s"] = s.pauseAfterS;
    } else {
      so["pause_after_s"] = nullptr;
    }
  }
  if (r && !r->steps.empty()) {
    JsonArray run = o["run_steps"].to<JsonArray>();
    for (size_t i = 0; i < r->steps.size(); ++i) {
      const StepRun& s = r->steps[i];
      JsonObject ro = run.add<JsonObject>();
      ro["device"] = s.device;
      ro["state"] = s.state;
      ro["attempts"] = s.attempts;
      if (!s.detail.isEmpty()) ro["detail"] = s.detail;
      if (static_cast<int>(i) == r->current) ro["current"] = true;
    }
  }
}

}  // namespace

void begin(esp_reset_reason_t reason, const char* reasonName, uint16_t delaySec) {
  if (!gMux) gMux = xSemaphoreCreateMutex();
  if (!gSaveMux) gSaveMux = xSemaphoreCreateMutex();
  load();
  refreshWatched();
  static bool watcherStarted = false;
  if (!watcherStarted) {
    watcherStarted = true;
    xTaskCreate(watcherTask, "scn_watch", 4096, nullptr, 1, nullptr);
  }

  // A software restart while the power-up's runs were still waiting for Wi-Fi (the watchdog in main.cpp): still a power-up.
  const bool carried = reason == ESP_RST_SW && gPowerUpPending == kPowerUpPending;
  const bool powerUp = reason == ESP_RST_POWERON || carried;
  if (carried) LOGW(TAG, "restarted before the power-up's scenarios got the network: they start again");
  bool pending = false;
  Lock lock;
  for (const Def& d : gDefs) {
    if (!d.enabled || d.trigger != kTriggerPowerOn) continue;
    if (!powerUp) {
      Runtime& r = gRun[d.name];
      r = Runtime();
      r.state = State::Skipped;
      r.skipReason = String("not a power-up (reset reason: ") + reasonName + ")";
      LOGI(TAG, "[%s] not started: %s", d.name.c_str(), r.skipReason.c_str());
    } else {
      LOGI(TAG, "[%s] power-up: starts %u s after boot", d.name.c_str(), static_cast<unsigned>(delaySec));
      if (startRun(d.name, static_cast<uint32_t>(delaySec) * 1000UL, "power-on")) {  // counted from boot
        gRun[d.name].holdsPowerUp = true;
        pending = true;
      }
    }
  }
  gPowerUpPending = pending ? kPowerUpPending : 0;
}

void sdAvailable() {
  if (!gMux) return;
  {
    Lock lock;
    if (gInfo.sd) return;  // the file was read at start (or after an earlier call)
  }
  LOGI(TAG, "the SD card is available now: reading %s", kPath);
  load();
  refreshWatched();
}

std::vector<String> missingDevices(const std::vector<String>& deviceNames) {
  std::vector<String> out;
  if (!gMux) return out;
  auto known = [&](const String& n) {
    for (const String& k : deviceNames) {
      if (k == n) return true;
    }
    return false;
  };
  Lock lock;
  for (const Def& d : gDefs) {
    if (d.isPower() && !known(d.whenDevice)) {
      out.push_back(String("scenario '") + d.name + "', trigger: device '" + d.whenDevice + "' is not in the list");
    }
    for (size_t i = 0; i < d.steps.size(); ++i) {
      if (d.steps[i].enabled && !known(d.steps[i].device)) {
        out.push_back(String("scenario '") + d.name + "', step " + String(static_cast<unsigned>(i + 1)) + ": device '" + d.steps[i].device +
                      "' is not in the list");
      }
    }
  }
  return out;
}

namespace {
RunResult runWith(const String& name, const char* trigger) {
  if (!gMux) return RunResult::NotFound;
  Lock lock;
  bool known = false;
  for (const Def& d : gDefs) known = known || d.name == name;
  if (!known) return RunResult::NotFound;
  auto it = gRun.find(name);
  if (it != gRun.end() && it->second.alive) {
    if (it->second.state == State::Waiting) {  // cut the wait short; the existing task carries on
      it->second.startAtMs = millis();
      it->second.trigger = trigger;
      LOGI(TAG, "[%s] %s start: the waiting run starts now", name.c_str(), trigger);
      return RunResult::Started;
    }
    return RunResult::AlreadyRunning;
  }
  LOGI(TAG, "[%s] start requested (%s)", name.c_str(), trigger);
  return startRun(name, millis(), trigger) ? RunResult::Started : RunResult::AlreadyRunning;
}
}  // namespace

RunResult runNow(const String& name) { return runWith(name, "manual"); }

StopResult stop(const String& name) {
  if (!gMux) return StopResult::NotFound;
  Lock lock;
  bool known = false;
  for (const Def& d : gDefs) known = known || d.name == name;
  if (!known) return StopResult::NotFound;
  auto it = gRun.find(name);
  if (it == gRun.end() || !it->second.alive) return StopResult::NotRunning;
  it->second.stop = true;
  LOGI(TAG, "[%s] stop requested", name.c_str());
  return StopResult::Stopping;
}

String configJson() {
  JsonDocument doc;
  doc["max"] = kMaxScenarios;
  doc["max_steps"] = kMaxSteps;
  doc["max_pause_s"] = kMaxPauseS;
  JsonArray triggers = doc["triggers"].to<JsonArray>();
  triggers.add(kTriggerPowerOn);
  triggers.add(kTriggerManual);
  triggers.add(kTriggerPowerBelow);
  triggers.add(kTriggerPowerAbove);
  {
    Lock lock;
    doc["sd"] = gInfo.sd;
    doc["file_found"] = gInfo.fileFound;
    if (!gInfo.error.isEmpty()) doc["error"] = gInfo.error;
    JsonArray arr = doc["scenarios"].to<JsonArray>();
    for (const Def& d : gDefs) {
      auto it = gRun.find(d.name);
      describe(arr.add<JsonObject>(), d, it == gRun.end() ? nullptr : &it->second, true);
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void summary(JsonArray out) {
  if (!gMux) return;
  Lock lock;
  for (const Def& d : gDefs) {
    auto it = gRun.find(d.name);
    describe(out.add<JsonObject>(), d, it == gRun.end() ? nullptr : &it->second, false);
  }
}

SaveResult save(const String& json) {
  SaveResult r;
  if (!gSaveMux) {
    r.error = "scenarios are not initialised";
    return r;
  }
  SaveLock saveLock;  // concurrent saves are serialised; readers are not blocked by the SD write
  if (!Storage::sdMounted()) {
    r.noSd = true;
    r.error = "no SD card: scenarios are stored only on the card";
    return r;
  }

  std::vector<Def> next;
  if (!parse(json, next, r.error)) {
    r.invalid = true;
    return r;
  }
  for (const Def& d : next) {  // a rule has to fit in the history of the readings (which holds a fixed number of answers)
    const uint32_t span = (Devices::kHistoryCapacity - 2) * static_cast<uint32_t>(Devices::pollIntervalSec());
    if (d.isPower() && d.forS > span) {
      r.invalid = true;
      r.error = String("scenario '") + d.name + "': when.for_s " + String(static_cast<unsigned>(d.forS)) + " s is longer than the history of the readings holds (" +
                String(static_cast<unsigned>(span)) + " s at the polling interval of " + String(static_cast<unsigned>(Devices::pollIntervalSec())) + " s)";
      return r;
    }
  }
  String text = fileText(next, true);  // readable on a PC; without the indentation if that does not fit
  if (text.length() > kMaxFileBytes) text = fileText(next, false);
  if (text.length() > kMaxFileBytes) {
    r.invalid = true;
    r.error = "the scenarios do not fit in the file (" + String(static_cast<unsigned>(kMaxFileBytes)) + " bytes): fewer scenarios or steps";
    return r;
  }

  // One generation of backup, so a wrong edit does not destroy the previous list.
  String old;
  if (Storage::sdReadAll(kPath, old, kMaxFileBytes) && !old.isEmpty()) {
    if (!Storage::sdWriteAtomic(kBak, kBakTmp, old)) LOGW(TAG, "could not write the backup %s", kBak);
  }
  if (!Storage::sdWriteAtomic(kPath, kTmp, text)) {
    r.error = "writing to the SD card failed, the scenarios are unchanged";
    LOGE(TAG, "%s", r.error.c_str());
    return r;
  }

  {
    Lock lock;
    for (auto it = gRun.begin(); it != gRun.end();) {
      const Def* now = nullptr;
      for (const Def& d : next) {
        if (d.name == it->first) now = &d;
      }
      if (!now) {  // removed: stop a live run, forget a finished one
        if (it->second.alive) {
          it->second.stop = true;
          ++it;
        } else {
          it = gRun.erase(it);
        }
        continue;
      }
      if (it->second.state == State::Skipped && (!now->enabled || now->trigger != kTriggerPowerOn)) it->second.state = State::Idle;
      ++it;
    }
    gDefs = next;
    gInfo.sd = true;
    gInfo.fileFound = true;
    gInfo.error = "";
  }
  refreshWatched();  // which devices need UPDATEDPS may have changed
  r.ok = true;
  r.count = next.size();
  LOGI(TAG, "scenarios saved from the web page: %u scenario(s)", static_cast<unsigned>(r.count));
  return r;
}

}  // namespace Scenario
