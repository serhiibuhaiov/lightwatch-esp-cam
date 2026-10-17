#include "scenario.h"

#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <WiFi.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "devices.h"
#include "engine.h"
#include "logger.h"
#include "psram_alloc.h"
#include "storage.h"
#include "tuya.h"

namespace Scenario {
namespace {

const char* TAG = "scenario";
const char* kPath = "/scenarios.json";
const char* kTmp = "/scenarios.tmp";
const char* kBak = "/scenarios.bak";
const char* kBakTmp = "/scenarios.bak.tmp";

// A step's command or status query (the engine retries; its timing is in lib/scenario_engine/engine.h).
constexpr uint32_t kCommandTimeoutMs = 3000;

const char* kTriggerPowerOn = "power_on";
const char* kTriggerManual = "manual";
const char* kTriggerPowerBelow = "power_below";
const char* kTriggerPowerAbove = "power_above";

// A power trigger: `when` = { device, watts, for_s, warmup_s } (limits in scenario.h).
constexpr uint32_t kDefaultWarmupS = 60;
constexpr uint32_t kWatchTickMs = 3000;  // how often the conditions are looked at

struct Step {
  uint16_t dev = 0;          // index into Def::devices
  bool on = true;            // the action: switch the device on (default) or off
  bool enabled = true;
  int32_t pauseAfterS = -1;  // -1: the scenario's default
};

struct Def {
  String name;
  bool enabled = true;
  String trigger = kTriggerPowerOn;
  uint16_t defaultPauseS = 30;
  std::vector<String> devices;  // the distinct device names of the steps (a step holds an index: one copy per name)
  std::vector<Step> steps;
  // power_below / power_above only:
  String whenDevice;
  uint32_t watts = 0;
  uint32_t forS = 0;
  uint32_t warmupS = kDefaultWarmupS;

  bool isPower() const { return trigger == kTriggerPowerBelow || trigger == kTriggerPowerAbove; }
  const String& device(const Step& s) const { return devices[s.dev]; }
};

// A power-up whose power_on runs are still waiting for the network is carried over a software restart: the Wi-Fi watchdog
// in main.cpp restarts the board (reset reason SW) when Wi-Fi stays away, and that must not lose the power-up. RTC memory
// keeps its value over a software restart and loses it when the power goes.
constexpr uint32_t kPowerUpPending = 0x50574F4E;  // "PWON"
RTC_NOINIT_ATTR uint32_t gPowerUpPending;

SemaphoreHandle_t gMux = nullptr;      // guards everything below
SemaphoreHandle_t gSaveMux = nullptr;  // serialises save(); held across the SD write
std::vector<Def> gDefs;
ScenarioEngine::Engine* gEngine = nullptr;  // every run of every scenario (lib/scenario_engine)
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

// The definitions changed: the engine stops the runs of scenarios that are gone. Caller holds gMux.
void pushDefs() {
  if (gEngine) gEngine->defsChanged();
}

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
  JsonDocument doc(PsramAllocator::instance());
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
      const String device = sv["device"].as<const char*>();
      size_t k = 0;
      while (k < d.devices.size() && d.devices[k] != device) ++k;
      if (k == d.devices.size()) d.devices.push_back(device);
      st.dev = static_cast<uint16_t>(k);
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
  JsonDocument doc(PsramAllocator::instance());
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
      so["device"] = d.device(s);
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
  if (replace) pushDefs();
  gInfo.sd = sd;
  gInfo.fileFound = fileFound;
  gInfo.error = error;
  if (error.isEmpty()) LOGI(TAG, "%u scenario(s) loaded from %s", static_cast<unsigned>(gDefs.size()), kPath);
}

// ---- running: the engine and the board's side of it ------------------------------------------------------------------

// What the engine sees of the board. Called by the engine task with gMux held: all of it is short, nothing waits for
// the network (Tuya::submit only queues; the jobs run in the Tuya workers).
class BoardEnv : public ScenarioEngine::Env {
 public:
  bool known(const std::string& name) override {
    for (const Def& d : gDefs) {
      if (d.name == name.c_str()) return true;
    }
    return false;
  }

  // The steps of a scenario as they are now, for a run that starts (the engine keeps no copy of the definitions).
  bool def(const std::string& name, ScenarioEngine::Def& out) override {
    for (const Def& d : gDefs) {
      if (d.name != name.c_str()) continue;
      out = ScenarioEngine::Def();
      out.name = name;
      out.defaultPauseS = d.defaultPauseS;
      out.devices.reserve(d.devices.size());
      for (const String& n : d.devices) out.devices.push_back(n.c_str());
      out.steps.reserve(d.steps.size());
      for (const Step& s : d.steps) {
        ScenarioEngine::StepDef st;
        st.dev = s.dev;
        st.on = s.on;
        st.enabled = s.enabled;
        st.pauseAfterS = s.pauseAfterS;
        out.steps.push_back(st);
      }
      return true;
    }
    return false;
  }

  uint32_t nowMs() override { return millis(); }
  bool wifiUp() override { return WiFi.status() == WL_CONNECTED; }

  ScenarioEngine::DeviceInfo device(const std::string& name) override {
    ScenarioEngine::DeviceInfo info;
    Devices::Device d;
    if (!Devices::find(String(name.c_str()), d)) return info;
    info.exists = true;
    info.ignored = d.ignore;
    Devices::State st;
    if (Devices::state(d.name, st)) info.offline = st.known && !st.online;
    info.switchState = Devices::switchState(d.name);
    return info;
  }

  uint32_t submit(ScenarioEngine::Op op, const std::string& name) override {
    std::shared_ptr<Tuya::Job> job;
    Devices::Device d;
    if (Devices::find(String(name.c_str()), d)) {
      const Tuya::Op top = op == ScenarioEngine::Op::On ? Tuya::Op::On : op == ScenarioEngine::Op::Off ? Tuya::Op::Off : Tuya::Op::Query;
      job = Tuya::submit(top, d.name, d.target(), d.dp, kCommandTimeoutMs, false);
      if (!job) return 0;  // the command list is full
    } else {  // removed from the list since the step began: a job that has failed already
      job = std::make_shared<Tuya::Job>();
      job->result.error = "device";
      job->result.detail = "the device is no longer in /devices.json";
      job->done.store(true, std::memory_order_release);
    }
    if (++nextId_ == 0) ++nextId_;  // 0 means "none"
    jobs_[nextId_] = job;
    return nextId_;
  }

  ScenarioEngine::JobStatus poll(uint32_t id) override {
    ScenarioEngine::JobStatus s;
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      s.done = true;
      s.error = "protocol";
      s.detail = "unknown job";
      return s;
    }
    const Tuya::Job& job = *it->second;
    s.done = job.done.load(std::memory_order_acquire);
    if (!s.done) return s;
    s.ok = job.result.ok;
    s.ms = job.result.ms;
    s.error = job.result.error.c_str();
    s.detail = job.result.detail.c_str();
    return s;
  }

  void release(uint32_t id, bool withdraw) override {
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    if (withdraw) it->second->cancelled.store(true);  // still queued: dropped, never reaches the socket
    jobs_.erase(it);
  }

  void log(bool warn, const std::string& line) override {
    Logger::log(warn ? Logger::Level::Warn : Logger::Level::Info, TAG, "%s", line.c_str());
  }

 private:
  std::map<uint32_t, std::shared_ptr<Tuya::Job>> jobs_;
  uint32_t nextId_ = 0;
};

BoardEnv* gEnv = nullptr;
TaskHandle_t gEngineTask = nullptr;

void wake() {
  if (gEngineTask) xTaskNotifyGive(gEngineTask);
}

void watchTick();  // the power rules, below

// One task drives every run (the engine) and looks at the power rules every kWatchTickMs.
void engineTask(void*) {
  uint32_t nextWatch = millis() + kWatchTickMs;
  for (;;) {
    uint32_t sleepMs;
    {
      Lock lock;
      sleepMs = gEngine->tick();
      // The power-up has been served once none of its runs waits for the network any more: a software restart from now on
      // must not repeat it.
      if (gPowerUpPending == kPowerUpPending && !gEngine->holdsPowerUp()) gPowerUpPending = 0;
    }
    const int32_t toWatch = static_cast<int32_t>(nextWatch - millis());
    if (toWatch <= 0) {
      nextWatch = millis() + kWatchTickMs;
      watchTick();  // takes the lock itself; reads the history without it
      continue;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<uint32_t>(sleepMs, static_cast<uint32_t>(toWatch))));
  }
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

// ---- JSON ------------------------------------------------------------------------------------

unsigned doneSteps(const ScenarioEngine::Run& r) {
  unsigned n = 0;
  for (const ScenarioEngine::StepRun& s : r.steps) {
    n += (s.state == ScenarioEngine::StepState::Ok || s.state == ScenarioEngine::StepState::Skipped ||
          s.state == ScenarioEngine::StepState::Failed || s.state == ScenarioEngine::StepState::Stopped) ? 1 : 0;
  }
  return n;
}

unsigned totalSteps(const ScenarioEngine::Run& r) {
  unsigned n = 0;
  for (const ScenarioEngine::StepRun& s : r.steps) n += s.state != ScenarioEngine::StepState::Disabled ? 1 : 0;
  return n;
}

uint32_t secondsUntil(uint32_t atMs) {
  const int32_t left = static_cast<int32_t>(atMs - millis());
  return left > 0 ? static_cast<uint32_t>(left) / 1000UL + 1 : 0;
}

// The caller holds the lock. Strings go in as std::string, so ArduinoJson copies them.
void describe(JsonObject o, const Def& d, const ScenarioEngine::Run* r, bool full) {
  using ScenarioEngine::RunState;
  o["name"] = d.name;
  o["enabled"] = d.enabled;
  o["trigger"] = d.trigger;
  const RunState st = r ? r->state : RunState::Idle;
  o["state"] = ScenarioEngine::runStateName(st);
  if (r) {
    if (st == RunState::Waiting) {
      o["starts_in_s"] = secondsUntil(r->startAtMs);
      if (!r->waitReason.empty()) o["wait_reason"] = r->waitReason;
    }
    if (st == RunState::Skipped) o["skip_reason"] = r->skipReason;
    if (st == RunState::Waiting || st == RunState::Running || st == RunState::Done || st == RunState::Stopped) o["run_trigger"] = r->trigger;
    if (!r->steps.empty()) {
      o["progress_done"] = doneSteps(*r);
      o["progress_total"] = totalSteps(*r);
    }
    if (r->alive && r->phase == ScenarioEngine::Phase::Pause) o["pause_left_s"] = secondsUntil(r->deadlineMs);
  }
  if (!full) return;
  o["default_pause_s"] = d.defaultPauseS;
  if (d.isPower()) {
    JsonObject w = o["when"].to<JsonObject>();
    w["device"] = d.whenDevice;
    w["watts"] = d.watts;
    w["for_s"] = d.forS;
    w["warmup_s"] = d.warmupS;
    auto it = gWatch.find(d.name);
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
    so["device"] = d.device(s);
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
      const ScenarioEngine::StepRun& s = r->steps[i];
      JsonObject ro = run.add<JsonObject>();
      ro["device"] = r->def.device(i);
      ro["state"] = ScenarioEngine::stepStateName(s.state);
      ro["attempts"] = s.attempts;
      const std::string detail = ScenarioEngine::stepDetail(*r, i);
      if (!detail.empty()) ro["detail"] = detail;
      if (static_cast<int>(i) == r->current) ro["current"] = true;
    }
  }
}

}  // namespace

void begin(esp_reset_reason_t reason, const char* reasonName, uint16_t delaySec) {
  if (!gMux) gMux = xSemaphoreCreateMutex();
  if (!gSaveMux) gSaveMux = xSemaphoreCreateMutex();
  {
    Lock lock;
    if (!gEngine) {
      gEnv = new BoardEnv();
      gEngine = new ScenarioEngine::Engine(*gEnv);
    }
  }
  load();
  refreshWatched();

  // A software restart while the power-up's runs were still waiting for Wi-Fi (the watchdog in main.cpp): still a power-up.
  const bool carried = reason == ESP_RST_SW && gPowerUpPending == kPowerUpPending;
  const bool powerUp = reason == ESP_RST_POWERON || carried;
  if (carried) LOGW(TAG, "restarted before the power-up's scenarios got the network: they start again");
  {
    Lock lock;
    bool pending = false;
    for (const Def& d : gDefs) {
      if (!d.enabled || d.trigger != kTriggerPowerOn) continue;
      if (!powerUp) {
        const String why = String("not a power-up (reset reason: ") + reasonName + ")";
        gEngine->skip(d.name.c_str(), why.c_str());
        LOGI(TAG, "[%s] not started: %s", d.name.c_str(), why.c_str());
      } else {
        LOGI(TAG, "[%s] power-up: starts %u s after boot", d.name.c_str(), static_cast<unsigned>(delaySec));
        // counted from boot
        if (gEngine->start(d.name.c_str(), "power-on", static_cast<uint32_t>(delaySec) * 1000UL, true) ==
            ScenarioEngine::Engine::StartResult::Started) {
          pending = true;
        }
      }
    }
    gPowerUpPending = pending ? kPowerUpPending : 0;
  }
  if (!gEngineTask) xTaskCreate(engineTask, "scn_engine", 8192, nullptr, 1, &gEngineTask);
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
      if (d.steps[i].enabled && !known(d.device(d.steps[i]))) {
        out.push_back(String("scenario '") + d.name + "', step " + String(static_cast<unsigned>(i + 1)) + ": device '" + d.device(d.steps[i]) +
                      "' is not in the list");
      }
    }
  }
  return out;
}

namespace {
RunResult runWith(const String& name, const char* trigger) {
  if (!gMux || !gEngine) return RunResult::NotFound;
  ScenarioEngine::Engine::StartResult r;
  {
    Lock lock;
    r = gEngine->start(name.c_str(), trigger, millis());
  }
  wake();
  switch (r) {
    case ScenarioEngine::Engine::StartResult::Started: return RunResult::Started;
    case ScenarioEngine::Engine::StartResult::AlreadyRunning: return RunResult::AlreadyRunning;
    default: return RunResult::NotFound;
  }
}
}  // namespace

RunResult runNow(const String& name) { return runWith(name, "manual"); }

StopResult stop(const String& name) {
  if (!gMux || !gEngine) return StopResult::NotFound;
  ScenarioEngine::Engine::StopResult r;
  {
    Lock lock;
    r = gEngine->stop(name.c_str());
  }
  wake();
  switch (r) {
    case ScenarioEngine::Engine::StopResult::Stopping: return StopResult::Stopping;
    case ScenarioEngine::Engine::StopResult::NotRunning: return StopResult::NotRunning;
    default: return StopResult::NotFound;
  }
}

void engineStats(JsonObject out) {
  if (!gMux || !gEngine) return;
  Lock lock;
  const ScenarioEngine::Stats& s = gEngine->stats();
  out["runs_alive"] = gEngine->alive();
  out["max_in_flight"] = ScenarioEngine::kMaxInFlight;
  out["in_flight"] = s.inFlight;
  out["max_in_flight_seen"] = s.maxInFlightSeen;
  out["jobs"] = s.jobs;
  out["slot_waits"] = s.slotWaits;
  out["slot_wait_max_ms"] = s.slotWaitMaxMs;
  out["queue_full"] = s.queueFull;
  out["offline_attempts"] = s.offlineAttempts;
  out["offline_attempts_ok"] = s.offlineAttemptsOk;
  out["offline_attempt_ms"] = s.offlineAttemptMs;
}

uint32_t maxForS() {
  const uint32_t span = (Devices::kHistoryCapacity - 2) * static_cast<uint32_t>(Devices::pollIntervalSec());
  return span < kMaxForS ? span : kMaxForS;
}

void limits(JsonObject out) {
  out["max"] = kMaxScenarios;
  out["max_steps"] = kMaxSteps;
  out["max_pause_s"] = kMaxPauseS;
  out["max_file_bytes"] = kMaxFileBytes;
  JsonObject p = out["power_rule"].to<JsonObject>();
  p["max_watts"] = kMaxWatts;
  p["min_for_s"] = kMinForS;
  p["max_for_s"] = maxForS();
  p["max_warmup_s"] = kMaxWarmupS;
  p["check_every_s"] = kWatchTickMs / 1000;
}

String configJson() {
  JsonDocument doc(PsramAllocator::instance());
  doc["max"] = kMaxScenarios;
  doc["max_steps"] = kMaxSteps;
  doc["max_pause_s"] = kMaxPauseS;
  limits(doc["limits"].to<JsonObject>());
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
    for (const Def& d : gDefs) describe(arr.add<JsonObject>(), d, gEngine ? gEngine->run(d.name.c_str()) : nullptr, true);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void summary(JsonArray out) {
  if (!gMux) return;
  Lock lock;
  for (const Def& d : gDefs) describe(out.add<JsonObject>(), d, gEngine ? gEngine->run(d.name.c_str()) : nullptr, false);
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
    gDefs = next;
    for (const Def& d : gDefs) {  // "not started: not a power-up" no longer fits a scenario that is not an enabled power_on one
      if (!d.enabled || d.trigger != kTriggerPowerOn) gEngine->unskip(d.name.c_str());
    }
    pushDefs();  // runs of the scenarios that are gone are stopped (alive) or forgotten
    gInfo.sd = true;
    gInfo.fileFound = true;
    gInfo.error = "";
  }
  wake();
  refreshWatched();  // which devices need UPDATEDPS may have changed
  r.ok = true;
  r.count = next.size();
  LOGI(TAG, "scenarios saved from the web page: %u scenario(s)", static_cast<unsigned>(r.count));
  return r;
}

}  // namespace Scenario
