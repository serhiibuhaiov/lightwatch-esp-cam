#include "scenario.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <time.h>

#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "devices.h"
#include "engine.h"
#include "http_out.h"
#include "http_template.h"
#include "logger.h"
#include "online_rule.h"
#include "power_rule.h"
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
// The runs that are alive, kept so that a restart can continue them (on_interrupt). Written on every change (at most every
// kRunsSaveGapMs) and every kRunsCheckpointMs while a run waits or pauses, so a pause loses at most that much.
const char* kRunsPath = "/runs.json";
const char* kRunsTmp = "/runs.tmp";
constexpr size_t kRunsMaxBytes = 131072;
constexpr uint32_t kRunsSaveGapMs = 3000;
constexpr uint32_t kRunsCheckpointMs = 300000;
constexpr size_t kWhyMaxChars = 160;  // a failure reason as /runs.json and the history keep it
// The history of the runs: one JSON line per ended run, appended; at kHistoryFileBytes the file becomes the old one.
const char* kHistoryDir = "/runs";
const char* kHistoryPath = "/runs/history.jsonl";
const char* kHistoryOld = "/runs/history.1.jsonl";

// A step's command or status query (the engine retries; its timing is in lib/scenario_engine/engine.h).
constexpr uint32_t kCommandTimeoutMs = 3000;

const char* kTriggerPowerOn = "power_on";
const char* kTriggerManual = "manual";
const char* kTriggerPowerBelow = "power_below";
const char* kTriggerPowerAbove = "power_above";
const char* kTriggerInterval = "interval";
const char* kTriggerRelay = "relay";
const char* kTriggerOnline = "online";
// The history of an interval scenario: a file of its own, a short line per run (see intervalFinished()).
const char* kIntervalDir = "/runs/interval";
constexpr size_t kIntervalPendingMax = 65536;  // lines waiting for the card (it is away): later ones are dropped

// A power trigger: `when` = { device, watts, for_s, warmup_s } (limits in scenario.h).
constexpr uint32_t kDefaultWarmupS = 60;
constexpr uint32_t kWatchTickMs = 3000;  // how often the conditions are looked at

struct Step {
  bool http = false;         // an HTTP step (action "http"): `dev` indexes Def::requests
  uint16_t waitS = 0;        // a wait step (action "wait"): its seconds (0: not a wait step)
  uint16_t dev = 0;          // index into Def::devices (or Def::requests)
  bool on = true;            // the action: switch the device on (default) or off
  bool enabled = true;
  int32_t pauseAfterS = -1;  // -1: the scenario's default
};

struct Def {
  String name;
  bool enabled = true;
  String trigger = kTriggerPowerOn;
  uint16_t defaultPauseS = 30;
  // In PSRAM when the board has it (ScenarioEngine::setAllocator in begin()), like the runs: 24 scenarios x 24 steps
  // would otherwise take ~16 KB of internal RAM.
  ScenarioEngine::List<String> devices;  // the distinct device names of the steps (a step holds an index: one copy per name)
  ScenarioEngine::List<ScenarioEngine::HttpReq> requests;  // the requests of the HTTP steps (URL up to 2 KB, body up to 16 KB)
  ScenarioEngine::List<Step> steps;
  // power_below / power_above / relay / online:
  String whenDevice;
  String whenTo;  // relay: "on" | "off" | "any"; online: "offline" | "online" | "any"
  uint32_t watts = 0;
  uint32_t forS = 0;  // power_below / power_above, online
  uint32_t warmupS = kDefaultWarmupS;
  uint32_t everyS = 0;  // interval only
  ScenarioEngine::OnInterrupt onInterrupt;  // what a restart does to a run of this scenario

  bool isPower() const { return trigger == kTriggerPowerBelow || trigger == kTriggerPowerAbove; }
  bool isInterval() const { return trigger == kTriggerInterval; }
  bool isRelay() const { return trigger == kTriggerRelay; }
  bool isOnline() const { return trigger == kTriggerOnline; }
  bool hasWhenDevice() const { return isPower() || isRelay() || isOnline(); }
  const String& device(const Step& s) const { return devices[s.dev]; }
};
using Defs = ScenarioEngine::List<Def>;

SemaphoreHandle_t gMux = nullptr;      // guards everything below
SemaphoreHandle_t gSaveMux = nullptr;  // serialises save(); held across the SD write
Defs gDefs;
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
  return t == kTriggerPowerOn || t == kTriggerManual || t == kTriggerPowerBelow || t == kTriggerPowerAbove || t == kTriggerInterval ||
         t == kTriggerRelay || t == kTriggerOnline;
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
bool parse(const String& raw, Defs& out, String& err) {
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
  if (arr.size() > maxScenarios()) {
    err = String("more than ") + String(static_cast<unsigned>(maxScenarios())) + " scenarios";
    return false;
  }

  Defs list;
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
      err = where + ".trigger must be \"power_on\", \"manual\", \"power_below\", \"power_above\", \"interval\", \"relay\" or \"online\"";
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
    if (d.isInterval()) {
      JsonVariantConst w = item["when"];
      JsonVariantConst e = w["every_s"];
      if (!w.is<JsonObjectConst>() || !e.is<int>() || e.as<int>() < static_cast<int>(kMinEveryS) || e.as<int>() > static_cast<int>(kMaxEveryS)) {
        err = where + ".when must be {\"every_s\": " + String(kMinEveryS) + ".." + String(kMaxEveryS) + "} (seconds) for the trigger interval";
        return false;
      }
      d.everyS = static_cast<uint32_t>(e.as<int>());
    }
    if (d.isRelay()) {
      JsonVariantConst w = item["when"];
      if (!w.is<JsonObjectConst>()) {
        err = where + ".when must be an object {device, to} for the trigger relay";
        return false;
      }
      if (!w["device"].is<const char*>() || !validName(w["device"].as<const char*>())) {
        err = where + ".when.device must be a device name (1..32 chars of a-z 0-9 _ -)";
        return false;
      }
      d.whenDevice = w["device"].as<const char*>();
      const String to = w["to"].is<const char*>() ? w["to"].as<const char*>() : "";
      if (to != "on" && to != "off" && to != "any") {
        err = where + ".when.to must be \"on\", \"off\" or \"any\"";
        return false;
      }
      d.whenTo = to;
    }
    if (d.isOnline()) {
      JsonVariantConst w = item["when"];
      if (!w.is<JsonObjectConst>()) {
        err = where + ".when must be an object {device, to, for_s} for the trigger online";
        return false;
      }
      if (!w["device"].is<const char*>() || !validName(w["device"].as<const char*>())) {
        err = where + ".when.device must be a device name (1..32 chars of a-z 0-9 _ -)";
        return false;
      }
      d.whenDevice = w["device"].as<const char*>();
      const String to = w["to"].is<const char*>() ? w["to"].as<const char*>() : "";
      if (to != "offline" && to != "online" && to != "any") {
        err = where + ".when.to must be \"offline\", \"online\" or \"any\"";
        return false;
      }
      d.whenTo = to;
      d.forS = 0;
      if (!w["for_s"].isNull()) {
        if (!w["for_s"].is<int>() || w["for_s"].as<int>() < 0 || w["for_s"].as<int>() > static_cast<int>(kMaxOnlineForS)) {
          err = where + ".when.for_s must be an integer 0.." + String(static_cast<unsigned>(kMaxOnlineForS)) + " (seconds)";
          return false;
        }
        d.forS = w["for_s"].as<int>();
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
      if (sv["action"] == "wait") {  // {action: "wait", seconds, enabled}: waits, always; no device, no pause after it
        if (!sv["device"].isNull() || !sv["url"].isNull() || !sv["pause_after_s"].isNull()) {
          err = at + ": a wait step has only seconds (and enabled): no device, url or pause_after_s";
          return false;
        }
        JsonVariantConst s = sv["seconds"];
        if (!s.is<int>() || s.as<int>() < 1 || s.as<int>() > static_cast<int>(ScenarioEngine::kMaxWaitS)) {
          err = at + ".seconds must be an integer 1.." + String(ScenarioEngine::kMaxWaitS) + " for a wait step";
          return false;
        }
        st.waitS = static_cast<uint16_t>(s.as<int>());
        if (!readBool(sv["enabled"], at + ".enabled", st.enabled, err)) return false;
        d.steps.push_back(st);
        continue;
      }
      if (!sv["seconds"].isNull()) {
        err = at + ".seconds is for a wait step (action \"wait\")";
        return false;
      }
      if (sv["action"] == "http") {  // {action: "http", method, url, content_type, body, timeout_ms, enabled, pause_after_s}
        if (!sv["device"].isNull()) {
          err = at + ".device is not for an http step";
          return false;
        }
        HttpOut::Job q;
        if (!HttpOut::parse(sv, q, err, at)) return false;
        std::string why;
        const bool hasWhen = d.hasWhenDevice();
        if (!HttpTemplate::validate(q.url.c_str(), q.url.size(), true, hasWhen, why)) {
          err = at + ".url: " + why.c_str();
          return false;
        }
        if (!HttpTemplate::validate(q.body.c_str(), q.body.size(), false, hasWhen, why)) {
          err = at + ".body: " + why.c_str();
          return false;
        }
        ScenarioEngine::HttpReq req;
        req.post = q.method == HttpOut::Method::Post;
        req.url.assign(q.url.c_str(), q.url.size());
        req.contentType.assign(q.contentType.c_str(), q.contentType.size());
        req.body.assign(q.body.c_str(), q.body.size());
        req.timeoutMs = q.timeoutMs;
        st.http = true;
        st.dev = static_cast<uint16_t>(d.requests.size());
        d.requests.push_back(std::move(req));
      } else if (!sv["method"].isNull() || !sv["url"].isNull() || !sv["body"].isNull() || !sv["content_type"].isNull() ||
                 !sv["timeout_ms"].isNull()) {
        err = at + ": method, url, content_type, body and timeout_ms are for an http step (action \"http\")";
        return false;
      }
      if (!st.http && (!sv["device"].is<const char*>() || !validName(sv["device"].as<const char*>()))) {
        err = at + ".device must be a device name (1..32 chars of a-z 0-9 _ -)";
        return false;
      }
      if (!st.http) {
        const String device = sv["device"].as<const char*>();
        size_t k = 0;
        while (k < d.devices.size() && d.devices[k] != device) ++k;
        if (k == d.devices.size()) d.devices.push_back(device);
        st.dev = static_cast<uint16_t>(k);
      }
      if (!st.http && !sv["action"].isNull()) {
        const String a = sv["action"].is<const char*>() ? String(sv["action"].as<const char*>()) : String("");
        if (a != "on" && a != "off") {
          err = at + ".action must be \"on\", \"off\", \"http\" or \"wait\"";
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
    JsonVariantConst oi = item["on_interrupt"];
    if (!oi.isNull()) {
      if (!oi.is<JsonObjectConst>()) {
        err = where + ".on_interrupt must be an object {power, software, crash, brownout, resume_within_min}";
        return false;
      }
      struct Slot { const char* key; ScenarioEngine::Policy* target; };
      const Slot slots[] = {{"power", &d.onInterrupt.power}, {"software", &d.onInterrupt.software}, {"crash", &d.onInterrupt.crash},
                            {"brownout", &d.onInterrupt.brownout}};
      for (const Slot& s : slots) {
        JsonVariantConst v = oi[s.key];
        if (v.isNull()) continue;
        if (!v.is<const char*>() || !ScenarioEngine::policyFromName(v.as<const char*>(), *s.target)) {
          err = where + ".on_interrupt." + s.key + " must be \"resume\", \"restart\" or \"drop\"";
          return false;
        }
      }
      JsonVariantConst m = oi["resume_within_min"];
      if (!m.isNull()) {
        if (!m.is<int>() || m.as<int>() < 0 || m.as<int>() > ScenarioEngine::kMaxResumeWithinMin) {
          err = where + ".on_interrupt.resume_within_min must be an integer 0.." + String(static_cast<unsigned>(ScenarioEngine::kMaxResumeWithinMin));
          return false;
        }
        d.onInterrupt.resumeWithinMin = static_cast<uint16_t>(m.as<int>());
      }
    }
    list.push_back(d);
  }
  out = list;
  return true;
}

void writeOnInterrupt(JsonObject o, const ScenarioEngine::OnInterrupt& p) {
  o["power"] = ScenarioEngine::policyName(p.power);
  o["software"] = ScenarioEngine::policyName(p.software);
  o["crash"] = ScenarioEngine::policyName(p.crash);
  o["brownout"] = ScenarioEngine::policyName(p.brownout);
  o["resume_within_min"] = p.resumeWithinMin;
}

// One step as the file and GET /api/scenarios have it.
void writeStep(JsonObject so, const Def& d, const Step& s) {
  if (s.waitS) {
    so["action"] = "wait";
    so["seconds"] = s.waitS;
  } else if (s.http) {
    const ScenarioEngine::HttpReq& q = d.requests[s.dev];
    so["action"] = "http";
    so["method"] = q.post ? "POST" : "GET";
    so["url"] = q.url.c_str();
    if (q.post) {
      so["content_type"] = q.contentType.empty() ? HttpOut::kDefaultContentType : q.contentType.c_str();
      so["body"] = q.body.c_str();
    }
    so["timeout_ms"] = q.timeoutMs;
  } else {
    so["device"] = d.device(s);
    so["action"] = s.on ? "on" : "off";
  }
  so["enabled"] = s.enabled;
}

String fileText(const Defs& list, bool pretty) {
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
    if (d.isInterval()) o["when"].to<JsonObject>()["every_s"] = d.everyS;
    if (d.isRelay() || d.isOnline()) {
      JsonObject w = o["when"].to<JsonObject>();
      w["device"] = d.whenDevice;
      w["to"] = d.whenTo;
      if (d.isOnline()) w["for_s"] = d.forS;
    }
    o["default_pause_s"] = d.defaultPauseS;
    writeOnInterrupt(o["on_interrupt"].to<JsonObject>(), d.onInterrupt);
    JsonArray steps = o["steps"].to<JsonArray>();
    for (const Step& s : d.steps) {
      JsonObject so = steps.add<JsonObject>();
      writeStep(so, d, s);
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
  Defs list;
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
    Defs tmpList;
    if (Storage::sdReadAll(kTmp, tmp, maxFileBytes()) && parse(tmp, tmpList, tmpErr) && Storage::sdReplace(kTmp, kPath)) {
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
    if (!Storage::sdReadAll(kPath, raw, maxFileBytes())) {
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
      out.requests = d.requests;
      out.steps.reserve(d.steps.size());
      for (const Step& s : d.steps) {
        ScenarioEngine::StepDef st;
        st.kind = s.waitS ? ScenarioEngine::StepKind::Wait : s.http ? ScenarioEngine::StepKind::Http : ScenarioEngine::StepKind::Device;
        st.dev = s.dev;
        st.waitS = s.waitS;
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
  uint32_t epoch() override {
    const time_t now = time(nullptr);
    return now > 1700000000 ? static_cast<uint32_t>(now) : 0;
  }
  void finished(const std::string& name, const ScenarioEngine::Run& r) override;  // below: the history line
  bool wifiUp() override { return WiFi.status() == WL_CONNECTED; }

  ScenarioEngine::DeviceInfo device(const std::string& name) override {
    ScenarioEngine::DeviceInfo info;
    Devices::Device d;
    if (!Devices::find(String(name.c_str()), d)) return info;
    info.exists = true;
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

  // Called by the engine with gMux held (the definitions are there for `when`).
  uint32_t submitHttp(const ScenarioEngine::HttpReq& req, bool quiet, const std::string& scenario, const std::string& trigger) override {
    auto job = std::make_shared<HttpOut::Job>();
    job->quiet = quiet;  // an interval scenario's ping: not a line in the usual log every time (intervalFinished reports the changes)
    job->method = req.post ? HttpOut::Method::Post : HttpOut::Method::Get;
    job->timeoutMs = req.timeoutMs;
    if (!job->url.assign(req.url.data(), req.url.size()) || !job->contentType.assign(req.contentType.data(), req.contentType.size()) ||
        !job->body.assign(req.body.data(), req.body.size())) {
      job->result.error = "memory";
      job->result.detail = "no memory for the request";
      job->done.store(true, std::memory_order_release);
    } else if (!fillRun(*job, scenario, trigger)) {
      job->done.store(true, std::memory_order_release);  // the error is in the result
    } else if (!HttpOut::submit(job)) {
      return 0;  // its list is full
    }
    if (++nextId_ == 0) ++nextId_;
    httpJobs_[nextId_] = job;
    return nextId_;
  }

  ScenarioEngine::JobStatus poll(uint32_t id) override {
    ScenarioEngine::JobStatus s;
    auto hj = httpJobs_.find(id);
    if (hj != httpJobs_.end()) {
      const HttpOut::Job& job = *hj->second;
      s.done = job.done.load(std::memory_order_acquire);
      if (!s.done) return s;
      const HttpOut::Result& r = job.result;
      s.ok = r.ok;
      s.ms = r.ms;
      s.error = r.error.c_str();
      if (r.ok) {
        s.detail = "answered " + std::to_string(r.status) + " in " + std::to_string(r.ms) + " ms";
      } else {
        s.detail = r.detail.c_str();
      }
      return s;
    }
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
    auto hj = httpJobs_.find(id);
    if (hj != httpJobs_.end()) {
      if (withdraw) hj->second->cancelled.store(true);  // still queued: dropped, never sent
      httpJobs_.erase(hj);
      return;
    }
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    if (withdraw) it->second->cancelled.store(true);  // still queued: dropped, never reaches the socket
    jobs_.erase(it);
  }

  // The placeholders of a run's request: its scenario, trigger and `when` device (below: needs findDef). False: the
  // reason is in the job's result.
  bool fillRun(HttpOut::Job& job, const std::string& scenario, const std::string& trigger);

  void log(bool warn, const std::string& line) override {
    Logger::log(warn ? Logger::Level::Warn : Logger::Level::Info, TAG, "%s", line.c_str());
  }
  void logQuiet(const std::string& line) override { Logger::log(Logger::Level::Debug, TAG, "%s", line.c_str()); }

 private:
  std::map<uint32_t, std::shared_ptr<Tuya::Job>> jobs_;
  std::map<uint32_t, std::shared_ptr<HttpOut::Job>> httpJobs_;  // the same id space as jobs_
  uint32_t nextId_ = 0;
};


// ---- the state of the power rules ---------------------------------------------------------------------------------------

// The state of a power rule, for the page and for "fire once": it fires when the condition becomes true and is armed
// again only after the condition has been false at a look. Whether it has fired is kept in /runs.json ("disarmed"), so a
// restart does not fire it a second time for the same condition. "False" means not-met or relay-off (PowerRule::rearms):
// an unanswered query, a gap or a history that is filling up do not arm it, unless its last run had a step that failed
// (the device was offline): then any "not met" arms it, and the run is tried again once the device is back.
struct Watch {
  bool armed = true;
  bool restored = false;  // restored "fired" after a restart: armed again only by a sure "condition false"
  bool pending = false;   // met while its scenario was running: starts when that run ends, if the condition still holds
  bool met = false;
  String reason = "no-data";
  uint32_t runS = 0;
  int32_t power = INT32_MIN;  // raw, 0.1 W
};
std::map<String, Watch> gWatch;  // by scenario name

// The state of a relay trigger: it starts its scenario once for every change of its device's switch (to `to`) seen after
// it began to watch (Devices::relaySeq numbers; see Devices::State::onSeq for what counts as a change). Several changes
// between two looks start one run. Nothing of it is kept over a restart: a change while the board was away is not seen.
struct RelayWatch {
  String device, to;            // what it watched when it began: an edit of `when` begins again from now
  uint32_t seen = 0;            // the changes up to this number are done with
  bool pending = false;         // changed while its scenario was running: starts when that run ends
  String pendingTrigger;        // relay-on | relay-off
  int relay = -1;               // the cached switch at the last look: 1 on, 0 off, -1 unknown
  String reason = "watching";   // watching | ignored | no-device
};
std::map<String, RelayWatch> gRelay;  // by scenario name

// The state of an online trigger (the decision is OnlineRule::look).
struct OnlineWatch {
  String device, to;            // what it watched when it began: an edit of `when` begins again from now
  uint32_t forS = 0;
  OnlineRule::Watch rule;
  bool pending = false;         // happened while its scenario was running: starts when that run ends
  String pendingTrigger;        // device-offline | device-online
  int online = -1;              // the socket at the last look: 1 online, 0 offline, -1 unknown
  String reason = "watching";   // watching | holding | not-seen-online | ignored | no-device
  uint32_t heldS = 0;
};
std::map<String, OnlineWatch> gOnline;  // by scenario name

// ---- the state of the interval scenarios -------------------------------------------------------------------------------

// The schedule and the counters since boot (in RAM: an interval scenario carries nothing over a restart).
struct Interval {
  uint32_t everyS = 0;     // the period the schedule was made for (an edit makes a new one)
  bool scheduled = false;
  bool first = false;      // the first tick of the schedule: it waits for the network instead of being skipped
  uint32_t nextMs = 0;     // the next tick
  uint32_t ok = 0, failed = 0;                    // ended runs
  uint32_t skippedBusy = 0, skippedNoWifi = 0;    // ticks that started nothing
  uint32_t failsInRow = 0;
  bool ended = false;      // a run has ended since boot
  uint32_t lastOk = 0, lastFailed = 0;            // Unix time (0: none, or no clock)
  String lastError;
};
std::map<String, Interval> gInterval;  // by scenario name
// Their history lines, by scenario, until they reach the card: a failure at once, successes every kIntervalFlushS.
std::map<String, String> gIntervalPending;
size_t gIntervalPendingBytes = 0;
bool gIntervalUrgent = false;
uint32_t gIntervalFlushedMs = 0;

String intervalPath(const String& name, bool old) { return String(kIntervalDir) + "/" + name + (old ? ".1.jsonl" : ".jsonl"); }

const Def* findDef(const String& name) {  // caller holds gMux
  for (const Def& d : gDefs) {
    if (d.name == name) return &d;
  }
  return nullptr;
}

// ---- /runs.json: the alive runs and the rules that have fired ------------------------------------------------------------

std::string clip(const std::string& s) { return s.size() > kWhyMaxChars ? s.substr(0, kWhyMaxChars) : s; }

// /runs.json for the alive runs and the power rules that have fired; `count` = how many of both (0: no file is needed).
// Caller holds gMux (the engine hands the runs over one at a time, the
// document lives in PSRAM: no copy of all runs in internal RAM, which the SD card's DMA needs at the same moment).
String runsTextLocked(uint32_t epoch, size_t& count) {
  JsonDocument doc(PsramAllocator::instance());
  doc["version"] = 1;
  doc["saved_epoch"] = epoch;
  JsonArray arr = doc["runs"].to<JsonArray>();
  count = 0;
  gEngine->forEachSnapshot([&](const ScenarioEngine::Snapshot& s) {
    ++count;
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    o["trigger"] = s.trigger;
    o["def_hash"] = s.defHash;
    o["state"] = static_cast<int>(s.state);
    o["phase"] = static_cast<int>(s.phase);
    o["patient"] = s.patient;
    o["step"] = s.step;
    o["attempt"] = s.attempt;
    o["left_ms"] = s.leftMs;
    o["run_ms"] = s.runMs;
    o["start_epoch"] = s.startEpoch;
    if (!s.resumed.empty()) o["resumed"] = s.resumed;
    o["switched"] = s.switched;
    o["skipped"] = s.skipped;
    o["failed"] = s.failed;
    JsonObject def = o["def"].to<JsonObject>();  // the steps the run works with: names and actions for the history
    def["default_pause_s"] = s.def.defaultPauseS;
    JsonArray devs = def["devices"].to<JsonArray>();
    for (const ScenarioEngine::Text& n : s.def.devices) devs.add(n.c_str());
    // An HTTP request only as "GET http://host": for the history's label. A run that is continued takes its steps from the
    // definitions (an edit since shows in def_hash), so the URL's path, the body and any token stay out of this file.
    JsonArray reqs = def["requests"].to<JsonArray>();
    for (const ScenarioEngine::HttpReq& q : s.def.requests) {
      JsonArray a = reqs.add<JsonArray>();  // [post, "http://host"]
      a.add(q.post);
      a.add("http://" + ScenarioEngine::hostOf(q.url));
    }
    JsonArray dsteps = def["steps"].to<JsonArray>();
    for (const ScenarioEngine::StepDef& st : s.def.steps) {
      JsonArray a = dsteps.add<JsonArray>();  // [dev, on, enabled, pause_after_s(, 1 = http | 2 = wait, wait_s)]
      a.add(st.dev);
      a.add(st.on);
      a.add(st.enabled);
      a.add(st.pauseAfterS);
      if (st.kind == ScenarioEngine::StepKind::Http) a.add(1);
      if (st.kind == ScenarioEngine::StepKind::Wait) {
        a.add(2);
        a.add(st.waitS);
      }
    }
    JsonArray steps = o["steps"].to<JsonArray>();
    for (size_t i = 0; i < s.steps.size(); ++i) {
      const ScenarioEngine::StepRun& sr = s.steps[i];
      JsonArray a = steps.add<JsonArray>();  // [state, attempts, pause_s, at_s, why]
      a.add(static_cast<int>(sr.state));
      a.add(sr.attempts);
      a.add(sr.pauseS);
      a.add(sr.atS);
      a.add(clip(ScenarioEngine::str(s.whys.get(i))));
    }
  });
  JsonArray disarmed = doc["disarmed"].to<JsonArray>();  // the power rules that fired and wait for their condition to go
  for (const auto& kv : gWatch) {
    if (kv.second.armed) continue;
    disarmed.add(kv.first);
    ++count;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// Reads the runs a restart interrupted and hands them to `fn` one at a time (with the time they were saved, 0 if unknown):
// 24 runs at once would be ~35 KB of internal RAM at boot. Nothing (and no error) when there is no card or no file.
void readRuns(const std::function<void(uint32_t savedEpoch, const ScenarioEngine::Snapshot&)>& fn, std::vector<String>& disarmed) {
  disarmed.clear();
  if (!Storage::sdMounted()) return;
  String raw;
  // A save removes the old file before renaming the new one: a power cut in between leaves only the temporary file.
  if (!Storage::sdReadAll(kRunsPath, raw, kRunsMaxBytes) && !Storage::sdReadAll(kRunsTmp, raw, kRunsMaxBytes)) return;
  JsonDocument doc(PsramAllocator::instance());
  if (deserializeJson(doc, raw)) {
    LOGW(TAG, "%s is not valid JSON: the interrupted runs are not continued", kRunsPath);
    return;
  }
  const uint32_t savedEpoch = doc["saved_epoch"] | 0u;
  for (JsonVariantConst n : doc["disarmed"].as<JsonArrayConst>()) {
    if (n.is<const char*>()) disarmed.push_back(n.as<const char*>());
  }
  for (JsonObjectConst o : doc["runs"].as<JsonArrayConst>()) {
    ScenarioEngine::Snapshot s;
    s.name = o["name"] | "";
    s.trigger = o["trigger"] | "manual";
    s.defHash = o["def_hash"] | 0u;
    s.state = static_cast<ScenarioEngine::RunState>(o["state"] | 0);
    s.phase = static_cast<ScenarioEngine::Phase>(o["phase"] | 0);
    s.patient = o["patient"] | false;
    s.step = o["step"] | 0u;
    s.attempt = o["attempt"] | 0;
    s.leftMs = o["left_ms"] | 0u;
    s.runMs = o["run_ms"] | 0u;
    s.startEpoch = o["start_epoch"] | 0u;
    s.resumed = o["resumed"] | "";
    s.switched = o["switched"] | 0u;
    s.skipped = o["skipped"] | 0u;
    s.failed = o["failed"] | 0u;
    JsonObjectConst def = o["def"];
    s.def.name = s.name;
    s.def.defaultPauseS = def["default_pause_s"] | 0;
    for (JsonVariantConst n : def["devices"].as<JsonArrayConst>()) s.def.devices.push_back(n.as<const char*>() ? n.as<const char*>() : "");
    for (JsonArrayConst a : def["requests"].as<JsonArrayConst>()) {
      ScenarioEngine::HttpReq q;
      q.post = a[0] | false;
      q.url = a[1] | "";
      s.def.requests.push_back(std::move(q));
    }
    for (JsonArrayConst a : def["steps"].as<JsonArrayConst>()) {
      ScenarioEngine::StepDef st;
      st.dev = a[0] | 0;
      st.on = a[1] | true;
      st.enabled = a[2] | true;
      st.pauseAfterS = a[3] | -1;
      const int kind = a[4] | 0;
      st.kind = kind == 1 ? ScenarioEngine::StepKind::Http : kind == 2 ? ScenarioEngine::StepKind::Wait : ScenarioEngine::StepKind::Device;
      st.waitS = a[5] | 0u;
      if (st.kind == ScenarioEngine::StepKind::Wait) {
        if (st.waitS < 1 || st.waitS > ScenarioEngine::kMaxWaitS) continue;  // damaged
      } else if (st.dev >= (st.kind == ScenarioEngine::StepKind::Http ? s.def.requests.size() : s.def.devices.size())) {
        continue;  // damaged
      }
      s.def.steps.push_back(st);
    }
    for (JsonArrayConst a : o["steps"].as<JsonArrayConst>()) {
      ScenarioEngine::StepRun sr;
      sr.state = static_cast<ScenarioEngine::StepState>(a[0] | 0);
      sr.attempts = a[1] | 0;
      sr.pauseS = a[2] | 0;
      sr.atS = a[3] | 0u;
      s.whys.set(s.steps.size(), a[4] | "");
      s.steps.push_back(sr);
    }
    if (!s.name.empty()) fn(savedEpoch, s);
  }
}

uint32_t gLastRunsSaveMs = 0;
bool gRunsDirty = false;
bool gRunsFileThere = false;  // so that an empty list removes the file once, not on every change

// Outside the lock: the card.
void saveRuns(const String& text, size_t count) {
  gLastRunsSaveMs = millis();
  if (!Storage::sdMounted()) return;
  if (!count) {
    if (gRunsFileThere) Storage::sdRemove(kRunsPath);
    gRunsFileThere = false;
    return;
  }
  if (text.length() > kRunsMaxBytes || !Storage::sdWriteAtomic(kRunsPath, kRunsTmp, text)) {
    LOGW(TAG, "could not save the alive runs to %s (%u bytes)", kRunsPath, static_cast<unsigned>(text.length()));
    return;
  }
  gRunsFileThere = true;
}

// ---- the history of the runs --------------------------------------------------------------------------------------------

uint32_t gHistoryNextId = 1;
// Lines made under gMux, written to the card by the engine task. One buffer, not a String per line: 24 runs ending at once
// would otherwise hold ~60 KB of small strings in internal RAM; a buffer over 4 KB goes to PSRAM by itself.
String gHistoryPending;
constexpr size_t kHistoryPendingMax = 65536;  // the card is away for long: later lines are dropped

void intervalFinished(const String& name, const ScenarioEngine::Run& r);  // below

bool BoardEnv::fillRun(HttpOut::Job& job, const std::string& scenario, const std::string& trigger) {
  const Def* d = findDef(String(scenario.c_str()));
  const String when = d && d->hasWhenDevice() ? d->whenDevice : String();
  return fillPlaceholders(job, String(scenario.c_str()), String(trigger.c_str()), when);
}

void BoardEnv::finished(const std::string& name, const ScenarioEngine::Run& r) {
  const Def* def = findDef(String(name.c_str()));
  if (def && def->isInterval()) {
    intervalFinished(def->name, r);
    return;
  }
  JsonDocument doc(PsramAllocator::instance());  // the key order matters: GET /api/scenarios/history finds a scenario by `{"id":N,"name":"..."`
  doc["id"] = gHistoryNextId++;
  doc["name"] = name;
  doc["trigger"] = r.trigger;
  doc["result"] = ScenarioEngine::runStateName(r.state);
  if (r.startEpoch) {
    doc["start"] = r.startEpoch;
  } else {
    doc["start"] = nullptr;
  }
  const uint32_t end = epoch();
  if (end) {
    doc["end"] = end;
  } else {
    doc["end"] = nullptr;
  }
  doc["duration_s"] = r.durationS;
  if (!r.resumed.empty()) doc["resumed"] = r.resumed;
  if (!r.skipReason.empty()) doc["reason"] = r.skipReason;
  doc["switched"] = r.switched;
  doc["skipped"] = r.skipped;
  doc["failed"] = r.failed;
  JsonArray steps = doc["steps"].to<JsonArray>();
  for (size_t i = 0; i < r.steps.size() && i < r.def.steps.size(); ++i) {
    const ScenarioEngine::StepRun& s = r.steps[i];
    JsonObject o = steps.add<JsonObject>();
    if (r.def.isWait(i)) {
      o["action"] = "wait";
      o["seconds"] = r.def.steps[i].waitS;
    } else if (r.def.isHttp(i)) {
      o["http"] = r.def.label(i);  // "POST api.telegram.org": never the path or the body
      o["action"] = "http";
    } else {
      o["device"] = r.def.device(i);
      o["action"] = r.def.steps[i].on ? "on" : "off";
    }
    o["state"] = ScenarioEngine::stepStateName(s.state);
    o["attempts"] = s.attempts;
    if (s.atS) o["at_s"] = s.atS;
    const std::string detail = clip(ScenarioEngine::stepDetail(r, i));
    if (!detail.empty()) o["detail"] = detail;
  }
  if (gHistoryPending.length() >= kHistoryPendingMax) return;
  String line;  // serializeJson() replaces the content of a String: one line at a time, then appended
  serializeJson(doc, line);
  if (gHistoryPending.isEmpty()) gHistoryPending.reserve(4096);  // > 4 KB: PSRAM when the board has it
  gHistoryPending += line;
  gHistoryPending += '\n';
}

// A run of an interval scenario: the counters, a line in the usual log only when it starts failing or works again, and a
// short history line: {"id","name","t" (start, Unix),"ok","s" (seconds),"steps":[[state, attempts(, why)]...]}, plus
// "trigger" when not "interval" and "result" when not "done". ~70 bytes for a healthcheck: 128 KB hold ~1800 runs.
void intervalFinished(const String& name, const ScenarioEngine::Run& r) {
  using ScenarioEngine::StepState;
  const bool ok = r.state == ScenarioEngine::RunState::Done && r.failed == 0;
  std::string why;
  for (size_t i = 0; i < r.steps.size() && i < r.def.steps.size(); ++i) {
    if (r.steps[i].state != StepState::Failed) continue;
    why = "step " + std::to_string(i + 1) + " " + r.def.label(i) + ": " + ScenarioEngine::stepDetail(r, i);
    break;
  }
  if (!ok && why.empty()) why = ScenarioEngine::runStateName(r.state);
  Interval& iv = gInterval[name];
  const time_t now = time(nullptr);
  const uint32_t epochNow = now > 1700000000 ? static_cast<uint32_t>(now) : 0;
  if (ok) {
    if (iv.failsInRow) {
      LOGI(TAG, "[%s] works again after %u failed run(s)", name.c_str(), static_cast<unsigned>(iv.failsInRow));
    } else if (!iv.ended) {
      LOGI(TAG, "[%s] first run since boot: ok (from now on a line here only when it starts failing)", name.c_str());
    }
    iv.failsInRow = 0;
    ++iv.ok;
    iv.lastOk = epochNow;
  } else {
    if (iv.failsInRow == 0) LOGW(TAG, "[%s] failing: %s", name.c_str(), why.c_str());
    ++iv.failsInRow;
    ++iv.failed;
    iv.lastFailed = epochNow;
    iv.lastError = clip(why).c_str();
  }
  iv.ended = true;

  JsonDocument doc(PsramAllocator::instance());  // the key order matters: history() finds a scenario by `{"id":N,"name":"..."`
  doc["id"] = gHistoryNextId++;
  doc["name"] = name;
  if (r.startEpoch) {
    doc["t"] = r.startEpoch;
  } else {
    doc["t"] = nullptr;
  }
  doc["ok"] = ok;
  doc["s"] = r.durationS;
  if (r.trigger != "interval") doc["trigger"] = r.trigger;
  if (r.state != ScenarioEngine::RunState::Done) doc["result"] = ScenarioEngine::runStateName(r.state);
  JsonArray steps = doc["steps"].to<JsonArray>();
  for (size_t i = 0; i < r.steps.size(); ++i) {
    const ScenarioEngine::StepRun& s = r.steps[i];
    JsonArray a = steps.add<JsonArray>();
    a.add(ScenarioEngine::stepStateName(s.state));
    a.add(s.attempts);
    if (s.state != StepState::Ok && s.state != StepState::Disabled && s.state != StepState::Pending) {
      const std::string detail = clip(ScenarioEngine::stepDetail(r, i));
      if (!detail.empty()) a.add(detail);
    }
  }
  if (gIntervalPendingBytes >= kIntervalPendingMax) return;
  String line;
  serializeJson(doc, line);
  line += '\n';
  gIntervalPending[name] += line;
  gIntervalPendingBytes += line.length();
  if (!ok) gIntervalUrgent = true;
}

void appendTo(const char* dir, const char* path, const char* oldPath, const String& lines) {
  if (lines.isEmpty() || !Storage::sdMounted()) return;
  Storage::sdMkdir(kHistoryDir);
  if (strcmp(dir, kHistoryDir) != 0) Storage::sdMkdir(dir);
  if (Storage::sdSize(path) > static_cast<long>(kHistoryFileBytes)) Storage::sdReplace(path, oldPath);
  if (!Storage::sdAppend(path, reinterpret_cast<const uint8_t*>(lines.c_str()), lines.length())) {
    LOGW(TAG, "could not write the history of the runs (%s)", path);
  }
}

void appendHistory(const String& lines) { appendTo(kHistoryDir, kHistoryPath, kHistoryOld, lines); }

// The id at the start of a history line, 0 if it is not one.
uint32_t lineId(const String& line) {
  if (!line.startsWith("{\"id\":")) return 0;
  return static_cast<uint32_t>(line.substring(6).toInt());
}

// The id of the last line of the newest of the two files, 0 if none.
uint32_t lastIdIn(const String& path, const String& oldPath) {
  for (const String& p : {path, oldPath}) {
    String tail;
    if (!Storage::sdReadTail(p.c_str(), 8192, tail)) continue;
    tail.trim();
    const int nl = tail.lastIndexOf('\n');
    const uint32_t id = lineId(nl >= 0 ? tail.substring(nl + 1) : tail);
    if (id) return id;
  }
  return 0;
}

// The ids of the history and of the interval scenarios' histories come from one counter (pages go by id): the highest of all.
uint32_t lastHistoryId() {
  uint32_t id = lastIdIn(kHistoryPath, kHistoryOld);
  std::vector<Storage::FileInfo> files;
  if (Storage::sdList(kIntervalDir, files)) {
    for (const Storage::FileInfo& f : files) {
      if (!f.name.endsWith(".jsonl") || f.name.endsWith(".1.jsonl")) continue;
      const String name = f.name.substring(0, f.name.length() - 6);
      id = std::max(id, lastIdIn(intervalPath(name, false), intervalPath(name, true)));
    }
  }
  return id;
}

BoardEnv* gEnv = nullptr;
TaskHandle_t gEngineTask = nullptr;

void wake() {
  if (gEngineTask) xTaskNotifyGive(gEngineTask);
}

void watchTick();  // the power rules, below
void relayTick();  // the relay triggers, below
void onlineTick();  // the online triggers, below
bool intervalTick();  // the interval scenarios, below

// One task drives every run (the engine) and looks at the power rules every kWatchTickMs.
void engineTask(void*) {
  uint32_t nextWatch = millis() + kWatchTickMs;
  for (;;) {
    uint32_t sleepMs;
    bool save = false;
    String runsText;
    size_t runsCount = 0;
    String lines;
    std::map<String, String> intervalLines;
    {
      Lock lock;
      sleepMs = gEngine->tick();
      if (gEngine->takeDirty()) gRunsDirty = true;
      const uint32_t sinceSave = millis() - gLastRunsSaveMs;
      if ((gRunsDirty && sinceSave >= kRunsSaveGapMs) || (gEngine->alive() && sinceSave >= kRunsCheckpointMs)) {
        const time_t now = time(nullptr);
        runsText = runsTextLocked(now > 1700000000 ? static_cast<uint32_t>(now) : 0, runsCount);
        save = true;
        gRunsDirty = false;
      } else if (gRunsDirty) {
        sleepMs = std::min<uint32_t>(sleepMs, kRunsSaveGapMs - sinceSave);
      }
      if (!gHistoryPending.isEmpty()) {
        lines = gHistoryPending;
        gHistoryPending = String();
      }
      if (!gIntervalPending.empty() &&
          (gIntervalUrgent || millis() - gIntervalFlushedMs >= kIntervalFlushS * 1000UL || gIntervalPendingBytes >= 8192)) {
        intervalLines.swap(gIntervalPending);
        gIntervalPendingBytes = 0;
        gIntervalUrgent = false;
        gIntervalFlushedMs = millis();
      }
    }
    // The card, outside the lock: the web page and the API never wait for it.
    if (save) saveRuns(runsText, runsCount);
    appendHistory(lines);
    for (const auto& kv : intervalLines) {
      appendTo(kIntervalDir, intervalPath(kv.first, false).c_str(), intervalPath(kv.first, true).c_str(), kv.second);
    }
    if (intervalTick()) continue;  // a run started: let the engine take it at once
    const int32_t toWatch = static_cast<int32_t>(nextWatch - millis());
    if (toWatch <= 0) {
      nextWatch = millis() + kWatchTickMs;
      watchTick();  // takes the lock itself; reads the history without it
      relayTick();
      onlineTick();
      continue;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<uint32_t>(sleepMs, static_cast<uint32_t>(toWatch))));
  }
}

// ---- power triggers ----------------------------------------------------------------------------


RunResult runWith(const String& name, const char* trigger);  // below

// The last run of `name` ended with a step that failed (not stopped by hand). The caller holds gMux.
bool lastRunFailed(const String& name) {
  const ScenarioEngine::Run* r = gEngine ? gEngine->run(name.c_str()) : nullptr;
  return r && r->state == ScenarioEngine::RunState::Done && r->failed > 0;
}

void watchTick() {
  struct Rule {  // what a look needs of an enabled power rule (not its steps: an HTTP step may carry 16 KB of body)
    String name, trigger, whenDevice;
    uint32_t watts, forS, warmupS;
  };
  std::vector<Rule> rules;
  {
    Lock lock;
    for (const Def& d : gDefs) {
      if (d.enabled && d.isPower()) rules.push_back(Rule{d.name, d.trigger, d.whenDevice, d.watts, d.forS, d.warmupS});
    }
    for (auto it = gWatch.begin(); it != gWatch.end();) {  // forget the rules that are gone or switched off
      bool present = false;
      for (const Rule& d : rules) present = present || d.name == it->first;
      if (!present && !it->second.armed) gRunsDirty = true;
      it = present ? std::next(it) : gWatch.erase(it);
    }
  }
  for (const Rule& d : rules) {
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
      if (!pc.met) w.pending = false;  // gone before the run ended: no repeat
      if (pc.met && w.armed) {
        fire = true;
      } else if (pc.met) {
        w.restored = false;  // fired, and the condition still holds: the usual "wait for it to go"
      } else if (PowerRule::rearms(w.reason.c_str()) || (!w.restored && lastRunFailed(d.name))) {
        if (!w.armed) gRunsDirty = true;  // /runs.json keeps the rules that fired
        w.armed = true;
        w.restored = false;
      }
    }
    if (!fire) continue;
    // Still running (an earlier run is not over): not started, the rule stays armed and is looked at again next time.
    const RunResult started = runWith(d.name, below ? "power-below" : "power-above");
    bool wasPending = false;
    bool nowPending = false;
    {
      Lock lock;
      Watch& w = gWatch[d.name];
      wasPending = w.pending;
      if (started == RunResult::Started) {
        w.armed = false;
        w.pending = false;
        gRunsDirty = true;
      } else if (started == RunResult::AlreadyRunning) {
        w.pending = true;
        nowPending = !wasPending;
      }
    }
    const char* what = below ? "power below the threshold" : "power above the threshold";
    if (started == RunResult::Started) {
      LOGI(TAG, "[%s] %s: %s held for %u s (>= %u s): started%s", d.name.c_str(), d.whenDevice.c_str(), what,
           static_cast<unsigned>(pc.runS), static_cast<unsigned>(d.forS), wasPending ? " after the run that was going" : "");
    } else if (nowPending) {
      LOGI(TAG, "[%s] %s: %s held for %u s (>= %u s) while the scenario is running: starts again when the run ends, if it still holds",
           d.name.c_str(), d.whenDevice.c_str(), what, static_cast<unsigned>(pc.runS), static_cast<unsigned>(d.forS));
    }
  }
}

// ---- relay triggers ---------------------------------------------------------------------------------------------------

void relayTick() {
  struct Rule {
    String name, device, to;
  };
  std::vector<Rule> rules;
  {
    Lock lock;
    for (const Def& d : gDefs) {
      if (d.enabled && d.isRelay()) rules.push_back(Rule{d.name, d.whenDevice, d.whenTo});
    }
    for (auto it = gRelay.begin(); it != gRelay.end();) {  // forget the triggers that are gone or switched off
      bool present = false;
      for (const Rule& d : rules) present = present || d.name == it->first;
      it = present ? std::next(it) : gRelay.erase(it);
    }
  }
  for (const Rule& d : rules) {
    const uint32_t now = Devices::relaySeq();  // taken before the state: a change in between is seen at the next look
    Devices::Device dev;
    Devices::State st;
    const bool found = Devices::find(d.device, dev) && Devices::state(d.device, st);
    uint32_t seq = 0;  // the newest change this trigger waits for
    bool on = false;
    if (found && !dev.ignore) {
      if (d.to != "off" && st.onSeq > seq) {
        seq = st.onSeq;
        on = true;
      }
      if (d.to != "on" && st.offSeq > seq) {
        seq = st.offSeq;
        on = false;
      }
    }
    String trigger;  // the run to start now ("" = none)
    bool changed = false;
    {
      Lock lock;
      RelayWatch& w = gRelay[d.name];
      if (w.device != d.device || w.to != d.to) {  // new, enabled again or edited: the changes from now on
        w = RelayWatch();
        w.device = d.device;
        w.to = d.to;
        w.seen = now;
      }
      w.relay = found ? Devices::switchState(d.device) : -1;
      w.reason = !found ? "no-device" : dev.ignore ? "ignored" : "watching";
      if (!found || dev.ignore) {
        w.seen = now;
        w.pending = false;
      } else if (seq > w.seen) {
        w.seen = seq;
        trigger = on ? "relay-on" : "relay-off";
        changed = true;
      } else if (w.pending) {
        trigger = w.pendingTrigger;
      }
    }
    if (trigger.isEmpty()) continue;
    const RunResult started = runWith(d.name, trigger.c_str());
    {
      Lock lock;
      RelayWatch& w = gRelay[d.name];
      if (started == RunResult::Started) {
        w.pending = false;
      } else if (started == RunResult::AlreadyRunning) {
        w.pending = true;
        w.pendingTrigger = trigger;
      }
    }
    const char* to = trigger == "relay-on" ? "ON" : "OFF";
    if (started == RunResult::Started) {
      LOGI(TAG, "[%s] %s: the switch went %s%s: started", d.name.c_str(), d.device.c_str(), to,
           changed ? "" : " during the run before");
    } else if (started == RunResult::AlreadyRunning && changed) {
      LOGI(TAG, "[%s] %s: the switch went %s while the scenario is running: starts again when the run ends", d.name.c_str(),
           d.device.c_str(), to);
    }
  }
}

// ---- online triggers ----------------------------------------------------------------------------------------------------

void onlineTick() {
  struct Rule {
    String name, device, to;
    uint32_t forS;
  };
  std::vector<Rule> rules;
  {
    Lock lock;
    for (const Def& d : gDefs) {
      if (d.enabled && d.isOnline()) rules.push_back(Rule{d.name, d.whenDevice, d.whenTo, d.forS});
    }
    for (auto it = gOnline.begin(); it != gOnline.end();) {  // forget the triggers that are gone or switched off
      bool present = false;
      for (const Rule& d : rules) present = present || d.name == it->first;
      it = present ? std::next(it) : gOnline.erase(it);
    }
  }
  for (const Rule& d : rules) {
    Devices::Device dev;
    Devices::State st;
    const bool found = Devices::find(d.device, dev) && Devices::state(d.device, st);
    OnlineRule::Device od;
    od.known = found && st.known;
    od.everOnline = st.everOnline;
    od.online = st.online;
    od.sinceMs = st.changedMs;
    const OnlineRule::To to = d.to == "offline" ? OnlineRule::To::Offline : d.to == "online" ? OnlineRule::To::Online : OnlineRule::To::Any;
    String trigger;  // the run to start now ("" = none)
    bool happened = false;
    {
      Lock lock;
      OnlineWatch& w = gOnline[d.name];
      if (w.device != d.device || w.to != d.to || w.forS != d.forS) {  // new, enabled again or edited: from the state now on
        w = OnlineWatch();
        w.device = d.device;
        w.to = d.to;
        w.forS = d.forS;
      }
      w.online = found && st.known ? (st.online ? 1 : 0) : -1;
      if (!found || dev.ignore) {
        w.rule = OnlineRule::Watch();
        w.pending = false;
        w.reason = !found ? "no-device" : "ignored";
        w.heldS = 0;
      } else {
        const OnlineRule::Look l = OnlineRule::look(w.rule, od, millis(), d.forS, to);
        w.reason = l.reason;
        w.heldS = l.heldS;
        if (l.fire >= 0) {
          trigger = l.fire ? "device-online" : "device-offline";
          happened = true;
        } else if (w.pending) {
          trigger = w.pendingTrigger;
        }
      }
    }
    if (trigger.isEmpty()) continue;
    const RunResult started = runWith(d.name, trigger.c_str());
    {
      Lock lock;
      OnlineWatch& w = gOnline[d.name];
      if (started == RunResult::Started) {
        w.pending = false;
      } else if (started == RunResult::AlreadyRunning) {
        w.pending = true;
        w.pendingTrigger = trigger;
      }
    }
    const char* what = trigger == "device-online" ? "came online" : "went offline";
    if (started == RunResult::Started) {
      LOGI(TAG, "[%s] %s %s%s: started", d.name.c_str(), d.device.c_str(), what, happened ? "" : " during the run before");
    } else if (started == RunResult::AlreadyRunning && happened) {
      LOGI(TAG, "[%s] %s %s while the scenario is running: starts again when the run ends", d.name.c_str(), d.device.c_str(), what);
    }
  }
}

// ---- interval triggers ---------------------------------------------------------------------------------------------------

// Starts the interval scenarios that are due; true if one was started. Called by the engine task on every look (about once
// a second). The first tick of a scenario comes kNetSettleMs after Wi-Fi is up, the next ones every every_s from the tick
// before (a fixed rate; one that fell behind starts from now, never a burst). A tick while the run of the previous one is
// still going, or without Wi-Fi, starts nothing and is counted.
bool intervalTick() {
  static bool wifiWasUp = false;
  static uint32_t wifiUpAtMs = 0;
  const uint32_t now = millis();
  const bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi && !wifiWasUp) wifiUpAtMs = now;
  wifiWasUp = wifi;
  const bool netReady = wifi && now - wifiUpAtMs >= ScenarioEngine::kNetSettleMs;
  bool started = false;
  Lock lock;
  for (auto it = gInterval.begin(); it != gInterval.end();) {  // forget the scenarios that are gone or have another trigger
    const Def* d = findDef(it->first);
    it = d && d->isInterval() ? std::next(it) : gInterval.erase(it);
  }
  for (const Def& d : gDefs) {
    if (!d.isInterval()) continue;
    Interval& iv = gInterval[d.name];
    if (!d.enabled) {
      iv.scheduled = false;
      continue;
    }
    const uint32_t periodMs = d.everyS * 1000UL;
    if (!iv.scheduled || iv.everyS != d.everyS) {  // new, enabled again or edited: the first tick as soon as the network allows
      iv.scheduled = true;
      iv.first = true;
      iv.everyS = d.everyS;
      iv.nextMs = now;
    }
    if (static_cast<int32_t>(now - iv.nextMs) < 0) continue;
    if (!netReady && iv.first) {  // after boot (or a new schedule): wait for Wi-Fi + 10 s, however long, then start
      iv.nextMs = wifi ? wifiUpAtMs + ScenarioEngine::kNetSettleMs : now + 1000;
      continue;
    }
    if (!wifi) {
      ++iv.skippedNoWifi;
      iv.nextMs = now + periodMs;
      continue;
    }
    if (!netReady) {
      iv.nextMs = wifiUpAtMs + ScenarioEngine::kNetSettleMs;
      continue;
    }
    iv.first = false;
    iv.nextMs += periodMs;
    if (static_cast<int32_t>(now - iv.nextMs) >= 0) iv.nextMs = now + periodMs;  // fell behind (a long run, no Wi-Fi)
    const ScenarioEngine::Run* r = gEngine->run(d.name.c_str());
    if (r && r->alive) {
      ++iv.skippedBusy;
      continue;
    }
    gEngine->start(d.name.c_str(), "interval", now, false, true);
    started = true;
  }
  return started;
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
// `full`: the definition too (on_interrupt, when, the power rule's state, the steps' run state); `withSteps`: also the steps
// as defined (HTTP steps carry their URL and body: up to 16 KB each, left out of the page's frequent polls).
void describe(JsonObject o, const Def& d, const ScenarioEngine::Run* r, bool full, bool withSteps = true) {
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
    if (st == RunState::Running && !r->waitReason.empty()) o["wait_reason"] = r->waitReason;
    if (st == RunState::Skipped) o["skip_reason"] = r->skipReason;
    if (st == RunState::Interrupted) o["interrupt_reason"] = r->skipReason;
    if (!r->resumed.empty()) o["resumed"] = r->resumed;
    if (st == RunState::Waiting || st == RunState::Running || st == RunState::Done || st == RunState::Stopped || st == RunState::Interrupted) {
      o["run_trigger"] = r->trigger;
    }
    if (!r->steps.empty()) {
      o["progress_done"] = doneSteps(*r);
      o["progress_total"] = totalSteps(*r);
    }
    if (r->alive && (r->phase == ScenarioEngine::Phase::Pause || r->phase == ScenarioEngine::Phase::Wait)) {
      o["pause_left_s"] = secondsUntil(r->deadlineMs);  // a pause after a step, or a wait step
    }
  }
  if (!full) return;
  o["default_pause_s"] = d.defaultPauseS;
  writeOnInterrupt(o["on_interrupt"].to<JsonObject>(), d.onInterrupt);
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
      ws["pending"] = it->second.pending;  // met during a run of this scenario: starts when that run ends
      if (it->second.restored) ws["restored"] = true;  // fired before the restart; armed again only by a sure "condition false"
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
  if (d.isOnline()) {
    JsonObject w = o["when"].to<JsonObject>();
    w["device"] = d.whenDevice;
    w["to"] = d.whenTo;
    w["for_s"] = d.forS;
    auto it = gOnline.find(d.name);
    if (it != gOnline.end()) {
      JsonObject ws = o["watch"].to<JsonObject>();
      ws["reason"] = it->second.reason;
      if (it->second.online < 0) {
        ws["online"] = nullptr;
      } else {
        ws["online"] = it->second.online == 1;
      }
      ws["held_s"] = it->second.heldS;     // how long the socket has been in its current state
      ws["pending"] = it->second.pending;  // happened during a run of this scenario: starts when that run ends
    }
  }
  if (d.isRelay()) {
    JsonObject w = o["when"].to<JsonObject>();
    w["device"] = d.whenDevice;
    w["to"] = d.whenTo;
    auto it = gRelay.find(d.name);
    if (it != gRelay.end()) {
      JsonObject ws = o["watch"].to<JsonObject>();
      ws["reason"] = it->second.reason;
      if (it->second.relay < 0) {
        ws["relay"] = nullptr;
      } else {
        ws["relay"] = it->second.relay == 1;
      }
      ws["pending"] = it->second.pending;  // changed during a run of this scenario: starts when that run ends
    }
  }
  if (d.isInterval()) {
    o["when"].to<JsonObject>()["every_s"] = d.everyS;
    auto it = gInterval.find(d.name);
    if (it != gInterval.end()) {
      const Interval& iv = it->second;
      JsonObject io = o["interval"].to<JsonObject>();  // since boot
      if (iv.scheduled) {
        io["next_in_s"] = secondsUntil(iv.nextMs);
      } else {
        io["next_in_s"] = nullptr;
      }
      io["ok"] = iv.ok;
      io["failed"] = iv.failed;
      io["fails_in_row"] = iv.failsInRow;
      io["skipped_busy"] = iv.skippedBusy;
      io["skipped_no_wifi"] = iv.skippedNoWifi;
      auto epochOrNull = [&](const char* key, uint32_t v) {
        if (v) {
          io[key] = v;
        } else {
          io[key] = nullptr;
        }
      };
      epochOrNull("last_ok", iv.lastOk);
      epochOrNull("last_failed", iv.lastFailed);
      if (!iv.lastError.isEmpty()) io["last_error"] = iv.lastError;
    }
  }
  if (withSteps) {
    JsonArray steps = o["steps"].to<JsonArray>();
    for (const Step& s : d.steps) {
      JsonObject so = steps.add<JsonObject>();
      writeStep(so, d, s);
      if (s.waitS) continue;  // a wait step has no pause after it
      if (s.pauseAfterS >= 0) {
        so["pause_after_s"] = s.pauseAfterS;
      } else {
        so["pause_after_s"] = nullptr;
      }
    }
  }
  if (r && !r->steps.empty()) {
    JsonArray run = o["run_steps"].to<JsonArray>();
    for (size_t i = 0; i < r->steps.size(); ++i) {
      const ScenarioEngine::StepRun& s = r->steps[i];
      JsonObject ro = run.add<JsonObject>();
      if (r->def.isWait(i)) {
        ro["wait_s"] = r->def.steps[i].waitS;
      } else if (r->def.isHttp(i)) {
        ro["http"] = r->def.label(i);
      } else {
        ro["device"] = r->def.device(i);
      }
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
  ScenarioEngine::setAllocator(&psramMalloc);  // the scenario list and the runs: PSRAM when the board has it
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

  using ScenarioEngine::Cause;
  using ScenarioEngine::Policy;
  const Cause cause = reason == ESP_RST_POWERON ? Cause::Power
                      : reason == ESP_RST_SW     ? Cause::Software
                      : reason == ESP_RST_BROWNOUT ? Cause::Brownout
                                                   : Cause::Crash;
  {
    Lock lock;
    gHistoryNextId = lastHistoryId() + 1;
    // The runs this restart interrupted, and how long the board was away (known when the clock was set before and after:
    // it survives a software restart, a panic and a watchdog, not a power cut).
    const time_t now = time(nullptr);
    // A power rule whose run is continued or started again has fired for this condition already: restored "fired".
    auto disarm = [&](const String& name) {
      for (const Def& x : gDefs) {
        if (x.name != name || !x.enabled || !x.isPower()) continue;
        Watch& w = gWatch[name];
        w.armed = false;
        w.restored = true;
        LOGI(TAG, "[%s] the power rule had fired before the restart: it fires again only after its condition was seen false", name.c_str());
      }
    };
    std::vector<String> disarmed;
    readRuns([&](uint32_t savedEpoch, const ScenarioEngine::Snapshot& s) {
      gRunsFileThere = true;
      const bool gapKnown = savedEpoch && now > 1700000000;
      const uint32_t gapS = gapKnown && static_cast<uint32_t>(now) > savedEpoch ? static_cast<uint32_t>(now) - savedEpoch : 0;
      const uint32_t elapsedMs = gapKnown ? gapS * 1000 : millis();
      const Def* d = nullptr;
      for (const Def& x : gDefs) {
        if (x.name == s.name.c_str()) d = &x;
      }
      if (!d) {
        gEngine->interrupted(s, "interrupted by a restart; the scenario no longer exists");
        return;
      }
      if (!d->enabled && s.trigger != "manual") {
        gEngine->interrupted(s, "interrupted by a restart; the scenario is disabled now");
        return;
      }
      bool defChanged = false;
      if (s.state == ScenarioEngine::RunState::Running) {
        ScenarioEngine::Def cur;
        defChanged = !gEnv->def(s.name, cur) || ScenarioEngine::defHash(cur) != s.defHash;
      }
      const ScenarioEngine::Decision dec = ScenarioEngine::decide(cause, d->onInterrupt, gapKnown, gapS, defChanged);
      const std::string why = std::string("interrupted by a restart (") + reasonName + "): " + dec.why;
      if (cause == Cause::Power && d->trigger == kTriggerPowerOn && dec.action != Policy::Resume) {
        gEngine->interrupted(s, why + "; the power-up starts it again");
      } else if (dec.action == Policy::Resume) {
        if (gEngine->resume(s, elapsedMs, reasonName) != ScenarioEngine::Engine::StartResult::Started) {
          gEngine->interrupted(s, why + "; could not be continued");
        } else if (s.trigger == "power-below" || s.trigger == "power-above") {
          disarm(d->name);
        }
      } else if (dec.action == Policy::Restart) {
        gEngine->interrupted(s, why);
        gEngine->start(s.name, s.trigger, millis(), true);
        if (s.trigger == "power-below" || s.trigger == "power-above") disarm(d->name);
      } else {
        gEngine->interrupted(s, why);
      }
    }, disarmed);
    if (!disarmed.empty()) gRunsFileThere = true;
    for (const String& name : disarmed) disarm(name);
    for (const Def& d : gDefs) {
      if (!d.enabled || d.trigger != kTriggerPowerOn) continue;
      const ScenarioEngine::Run* r = gEngine->run(d.name.c_str());
      if (r && r->alive) continue;  // continued (or started again) after the restart
      if (reason != ESP_RST_POWERON) {
        const String why = String("not a power-up (reset reason: ") + reasonName + ")";
        gEngine->skip(d.name.c_str(), why.c_str());
        LOGI(TAG, "[%s] not started: %s", d.name.c_str(), why.c_str());
      } else {
        LOGI(TAG, "[%s] power-up: starts %u s after boot", d.name.c_str(), static_cast<unsigned>(delaySec));
        gEngine->start(d.name.c_str(), "power-on", static_cast<uint32_t>(delaySec) * 1000UL);  // counted from boot
      }
    }
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
    if (d.hasWhenDevice() && !known(d.whenDevice)) {
      out.push_back(String("scenario '") + d.name + "', trigger: device '" + d.whenDevice + "' is not in the list");
    }
    for (size_t i = 0; i < d.steps.size(); ++i) {
      if (d.steps[i].enabled && !d.steps[i].http && !d.steps[i].waitS && !known(d.device(d.steps[i]))) {
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
    const Def* d = findDef(name);
    r = gEngine->start(name.c_str(), trigger, millis(), false, d && d->isInterval());  // an interval scenario's run is quiet
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
  out["max_http_in_flight"] = ScenarioEngine::kMaxHttpInFlight;
  out["http_requests"] = s.httpRequests;
  out["offline_attempts"] = s.offlineAttempts;
  out["offline_attempts_ok"] = s.offlineAttemptsOk;
  out["offline_attempt_ms"] = s.offlineAttemptMs;
}

bool history(const String& name, size_t limit, uint32_t before, bool failedOnly, String& out) {
  out = "{\"runs\":[";
  // An interval scenario has a history of its own, and its newest lines may still wait in RAM for the card.
  bool interval = false;
  String pending;
  if (!name.isEmpty() && gMux) {
    Lock lock;
    const Def* d = findDef(name);
    interval = d && d->isInterval();
    if (interval) {
      auto it = gIntervalPending.find(name);
      if (it != gIntervalPending.end()) pending = it->second;
    }
  }
  const bool sd = Storage::sdMounted();
  const String key = name.isEmpty() ? String() : ",\"name\":\"" + name + "\",";
  size_t found = 0;
  bool more = false;
  // false once `limit` lines are out and there is one more.
  auto take = [&](const String& line) {
    const uint32_t id = lineId(line);
    if (!id || (before && id >= before)) return true;
    if (!key.isEmpty() && line.indexOf(key) < 0) return true;
    if (failedOnly && line.indexOf("\"ok\":false") < 0) return true;
    if (found == limit) {
      more = true;
      return false;
    }
    if (found) out += ',';
    out += line;
    ++found;
    return true;
  };
  int end = pending.length();  // the waiting lines, last first
  while (end > 0 && !more) {
    int nl = pending.lastIndexOf('\n', end - 2);
    take(pending.substring(nl + 1, pending[end - 1] == '\n' ? end - 1 : end));
    end = nl + 1;
  }
  const String path = interval ? intervalPath(name, false) : String(kHistoryPath);
  const String oldPath = interval ? intervalPath(name, true) : String(kHistoryOld);
  // Newest first: the current file from its end, then the old one. Read in blocks, so a big file never sits in memory.
  for (const String& file : {path, oldPath}) {
    if (!sd || more) break;
    long pos = Storage::sdSize(file.c_str());
    if (pos <= 0) continue;
    String carry;  // the start of a line whose beginning is in an earlier block
    uint8_t buf[1024];
    while (pos > 0 && !more) {
      const size_t n = static_cast<size_t>(std::min<long>(pos, sizeof(buf)));
      pos -= static_cast<long>(n);
      const size_t got = Storage::sdReadAt(file.c_str(), static_cast<size_t>(pos), buf, n);
      if (got != n) break;
      String chunk;
      chunk.concat(reinterpret_cast<const char*>(buf), static_cast<unsigned int>(n));
      chunk += carry;
      int start = 0;
      if (pos > 0) {  // the first line may begin in the block before this one
        start = chunk.indexOf('\n');
        if (start < 0) {
          carry = chunk;
          continue;
        }
        carry = chunk.substring(0, start);
        ++start;
      } else {
        carry = "";
      }
      int stop = chunk.length();
      while (stop > start && !more) {  // the lines of this block, last first
        int nl = chunk.lastIndexOf('\n', stop - 1);
        if (nl < start - 1) nl = start - 1;
        const String line = chunk.substring(nl + 1, stop);
        stop = nl;
        take(line);
      }
    }
  }
  out += "],\"more\":";
  out += more ? "true" : "false";
  out += interval ? ",\"interval\":true" : "";
  out += sd ? ",\"sd\":true}" : ",\"sd\":false}";
  return sd;
}

bool fillPlaceholders(HttpOut::Job& job, const String& scenario, const String& trigger, const String& whenDevice) {
  char now[24] = "-";
  const time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%S", &lt);
  }
  auto resolve = [&](const HttpTemplate::Ref& r) -> std::string {
    using Kind = HttpTemplate::Ref::Kind;
    switch (r.kind) {
      case Kind::Time: return now;
      case Kind::Scenario: return scenario.isEmpty() ? "-" : scenario.c_str();
      case Kind::Trigger: return trigger.isEmpty() ? "-" : trigger.c_str();
      case Kind::Device:
      case Kind::When: {
        const String name = r.kind == Kind::When ? whenDevice : String(r.device.c_str());
        if (r.field == HttpTemplate::Field::EnergyIntegrated || r.field == HttpTemplate::Field::EnergyReported) {
          // The energy since boot: a sum, so it is there while the socket is offline too (it is what was counted).
          Devices::Energy e;
          int32_t wh = HttpTemplate::kNone;
          if (!name.isEmpty() && Devices::energy(name, e)) {
            if (r.field == HttpTemplate::Field::EnergyIntegrated && e.integratedKnown) {
              wh = static_cast<int32_t>(std::lround(e.integratedWh));
            } else if (r.field == HttpTemplate::Field::EnergyReported && e.reportedKnown) {
              wh = static_cast<int32_t>(std::min<uint64_t>(e.reportedWh, INT32_MAX));
            }
          }
          return HttpTemplate::format(r.field, wh);
        }
        int32_t v[4];
        int relay = -1;
        if (!name.isEmpty()) Devices::readings(name, v, relay);
        if (r.field == HttpTemplate::Field::Relay) return HttpTemplate::format(r.field, relay);
        return name.isEmpty() ? "-" : HttpTemplate::format(r.field, v[static_cast<int>(r.field)]);
      }
    }
    return "-";
  };
  struct Part {
    HttpOut::Text& text;
    size_t max;
    const char* name;
  } parts[] = {{job.url, HttpOut::kMaxUrlBytes, "url"}, {job.body, HttpOut::kMaxBodyBytes, "body"}};
  for (Part& p : parts) {
    if (!strstr(p.text.c_str(), "{{")) continue;  // nothing to fill in (the text holds no NUL: it came from JSON)
    std::string out;
    if (!HttpTemplate::expand(p.text.c_str(), p.text.size(), resolve, p.max, out)) {
      job.result.error = p.name;
      job.result.detail = String("longer than ") + static_cast<unsigned>(p.max) + " bytes with the placeholders filled in";
      return false;
    }
    if (!p.text.assign(out.data(), out.size())) {
      job.result.error = "memory";
      job.result.detail = "no memory for the request";
      return false;
    }
  }
  return true;
}

size_t maxScenarios() { return psramAvailable() ? kMaxScenariosPsram : kMaxScenariosNoPsram; }
size_t maxFileBytes() { return psramAvailable() ? kMaxFileBytesPsram : kMaxFileBytesNoPsram; }
size_t maxBodyBytes() { return psramAvailable() ? kMaxBodyBytesPsram : kMaxBodyBytesNoPsram; }

uint32_t maxForS() {
  const uint32_t span = (Devices::kHistoryCapacity - 2) * static_cast<uint32_t>(Devices::pollIntervalSec());
  return span < kMaxForS ? span : kMaxForS;
}

void limits(JsonObject out) {
  out["basis"] = psramAvailable() ? "psram" : "internal";  // what the three limits below were chosen for
  out["max"] = maxScenarios();
  out["max_with_psram"] = kMaxScenariosPsram;
  out["max_without_psram"] = kMaxScenariosNoPsram;
  out["max_body_bytes"] = maxBodyBytes();
  out["max_steps"] = kMaxSteps;
  out["max_pause_s"] = kMaxPauseS;
  out["max_wait_s"] = ScenarioEngine::kMaxWaitS;  // a wait step: 1..max_wait_s
  out["max_file_bytes"] = maxFileBytes();
  out["history_file_bytes"] = kHistoryFileBytes;  // two files: the current one and the one before
  out["max_resume_within_min"] = ScenarioEngine::kMaxResumeWithinMin;
  JsonObject p = out["power_rule"].to<JsonObject>();
  p["max_watts"] = kMaxWatts;
  p["min_for_s"] = kMinForS;
  p["max_for_s"] = maxForS();
  p["max_warmup_s"] = kMaxWarmupS;
  p["check_every_s"] = kWatchTickMs / 1000;
  JsonObject iv = out["interval"].to<JsonObject>();
  iv["min_every_s"] = kMinEveryS;
  iv["max_every_s"] = kMaxEveryS;
  iv["history_flush_s"] = kIntervalFlushS;  // successes reach the card this often, a failure at once
  iv["history_file_bytes"] = kHistoryFileBytes;  // per scenario, two files
  out["online"].to<JsonObject>()["max_for_s"] = kMaxOnlineForS;  // trigger "online": when.for_s 0..max_for_s
  JsonObject h = out["http_step"].to<JsonObject>();  // an HTTP step's request (the same limits as POST /api/http/test)
  h["max_url_bytes"] = HttpOut::kMaxUrlBytes;
  h["max_body_bytes"] = HttpOut::kMaxBodyBytes;
  h["max_content_type_bytes"] = HttpOut::kMaxContentTypeBytes;
  h["timeout_ms"] = HttpOut::kDefaultTimeoutMs;
  h["min_timeout_ms"] = HttpOut::kMinTimeoutMs;
  h["max_timeout_ms"] = HttpOut::kMaxTimeoutMs;
  h["max_in_flight"] = ScenarioEngine::kMaxHttpInFlight;  // requests of all scenarios in the HTTP lane at once
}

String configJson(bool stateOnly) {
  JsonDocument doc(PsramAllocator::instance());
  doc["max"] = maxScenarios();
  doc["max_steps"] = kMaxSteps;
  doc["max_pause_s"] = kMaxPauseS;
  limits(doc["limits"].to<JsonObject>());
  JsonArray triggers = doc["triggers"].to<JsonArray>();
  triggers.add(kTriggerPowerOn);
  triggers.add(kTriggerManual);
  triggers.add(kTriggerPowerBelow);
  triggers.add(kTriggerPowerAbove);
  triggers.add(kTriggerInterval);
  triggers.add(kTriggerRelay);
  triggers.add(kTriggerOnline);
  {
    Lock lock;
    doc["sd"] = gInfo.sd;
    doc["file_found"] = gInfo.fileFound;
    if (!gInfo.error.isEmpty()) doc["error"] = gInfo.error;
    JsonArray arr = doc["scenarios"].to<JsonArray>();
    for (const Def& d : gDefs) describe(arr.add<JsonObject>(), d, gEngine ? gEngine->run(d.name.c_str()) : nullptr, true, !stateOnly);
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

  Defs next;
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
  if (text.length() > maxFileBytes()) text = fileText(next, false);
  if (text.length() > maxFileBytes()) {
    r.invalid = true;
    r.error = "the scenarios do not fit in the file (" + String(static_cast<unsigned>(maxFileBytes())) + " bytes): fewer scenarios or steps";
    return r;
  }

  // One generation of backup, so a wrong edit does not destroy the previous list.
  String old;
  if (Storage::sdReadAll(kPath, old, maxFileBytes()) && !old.isEmpty()) {
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
  r.ok = true;
  r.count = next.size();
  LOGI(TAG, "scenarios saved from the web page: %u scenario(s)", static_cast<unsigned>(r.count));
  return r;
}

}  // namespace Scenario
