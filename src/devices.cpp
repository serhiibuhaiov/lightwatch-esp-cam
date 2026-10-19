#include "devices.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <WiFi.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <map>

#include "logger.h"
#include "power_rule.h"
#include "psram_alloc.h"
#include "storage.h"

namespace Devices {
namespace {

const char* TAG = "devices";
const char* kPath = "/devices.json";
const char* kTmp = "/devices.tmp";
constexpr size_t kMaxFileBytes = 8192;
const char* kBak = "/devices.bak";
const char* kBakTmp = "/devices.bak.tmp";

SemaphoreHandle_t gMux = nullptr;
std::vector<Device> gList;
LoadInfo gInfo;
std::map<String, State> gStates;  // by device name
uint32_t gRelaySeq = 0;  // see relaySeq()

// One finished status query (see power_rule.h): values INT32_MIN = none, flags ok / relay known / relay on.
using Sample = PowerRule::Sample;
struct History {
  std::vector<Sample, PsramStdAllocator<Sample>> buf;  // a ring once it is full; ~1.9 KB, in PSRAM when there is some
  size_t head = 0;          // the oldest sample when full
};
std::map<String, History> gHistory;  // by device name; lives and dies with the state
uint16_t gIntervalSec = 30;
std::atomic<uint32_t> gLastCycleMs{0}, gMaxCycleMs{0};  // see monitorCycle()

// Seconds since boot from the 64-bit timer: millis()/1000 would jump back to 0 after 49.7 days and break the history.
uint32_t uptimeS() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

struct Lock {
  Lock() {
    if (!gMux) gMux = xSemaphoreCreateMutex();
    xSemaphoreTake(gMux, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(gMux); }
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

SemaphoreHandle_t gSaveMux = nullptr;  // serialises save(); held across the SD write

struct SaveLock {
  SaveLock() { xSemaphoreTake(gSaveMux, portMAX_DELAY); }
  ~SaveLock() { xSemaphoreGive(gSaveMux); }
};

// Parses the whole document; on success `out` holds the list, on failure `err` says why.
// `keep` (the current list) lets a device with an empty or missing key inherit the key of the
// device with the same name; without it the key is mandatory (the file on the card).
bool parse(const String& raw, std::vector<Device>& out, String& err, const std::vector<Device>* keep) {
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
  JsonVariantConst arr = doc["devices"];
  if (!arr.is<JsonArrayConst>()) {
    err = "\"devices\" must be an array";
    return false;
  }
  if (arr.size() > kMaxDevices) {
    err = String("more than ") + String(static_cast<unsigned>(kMaxDevices)) + " devices";
    return false;
  }

  std::vector<Device> list;
  size_t index = 0;
  for (JsonVariantConst item : arr.as<JsonArrayConst>()) {
    const String where = String("devices[") + String(static_cast<unsigned>(index++)) + "]";
    if (!item.is<JsonObjectConst>()) {
      err = where + " must be an object";
      return false;
    }
    Device d;

    if (!item["name"].is<const char*>()) {
      err = where + ".name must be a string";
      return false;
    }
    d.name = item["name"].as<const char*>();
    if (!validName(d.name)) {
      err = where + ".name must be 1..32 chars of a-z 0-9 _ -";
      return false;
    }
    for (const Device& other : list) {
      if (other.name == d.name) {
        err = where + ".name '" + d.name + "' is used twice";
        return false;
      }
    }

    IPAddress ip;
    if (!item["ip"].is<const char*>() || !ip.fromString(item["ip"].as<const char*>())) {
      err = where + ".ip must be an IPv4 address like 192.168.1.20";
      return false;
    }
    d.ip = item["ip"].as<const char*>();

    const char* keyText = item["key"].is<const char*>() ? item["key"].as<const char*>() : "";
    if (keyText[0] == '\0' && keep) {
      bool found = false;
      for (const Device& k : *keep) {
        if (k.name == d.name) {
          d.key = k.key;
          found = true;
        }
      }
      if (!found) {
        err = where + ".key is required for a new or renamed device (16 characters)";
        return false;
      }
    } else if (strlen(keyText) != 16) {
      err = where + ".key must be a string of exactly 16 characters";
      return false;
    } else {
      d.key = keyText;
    }

    const String ver = item["version"].is<const char*>() ? String(item["version"].as<const char*>()) : String("");
    if (ver == "3.4") {
      d.version = Tuya::Version::V34;
    } else if (ver == "3.5") {
      d.version = Tuya::Version::V35;
    } else {
      err = where + ".version must be \"3.4\" or \"3.5\"";
      return false;
    }

    if (!item["dp"].isNull()) {
      if (!item["dp"].is<long>() || item["dp"].as<long>() < 1 || item["dp"].as<long>() > 255) {
        err = where + ".dp must be an integer 1..255";
        return false;
      }
      d.dp = static_cast<uint16_t>(item["dp"].as<long>());
    }
    if (!item["ignore"].isNull()) {
      if (!item["ignore"].is<bool>()) {
        err = where + ".ignore must be true or false";
        return false;
      }
      d.ignore = item["ignore"].as<bool>();
    }
    if (!item["keep_connected"].isNull()) {
      if (!item["keep_connected"].is<bool>()) {
        err = where + ".keep_connected must be true or false";
        return false;
      }
      d.keepConnected = item["keep_connected"].as<bool>();
    }
    if (!item["updatedps"].isNull()) {
      if (!item["updatedps"].is<bool>()) {
        err = where + ".updatedps must be true or false";
        return false;
      }
      d.updatedps = item["updatedps"].as<bool>();
    }
    // "monitor" ("poll"/"push") of older firmware is not read any more: poll_s says how often the device is queried.
    if (!item["poll_s"].isNull()) {
      if (!item["poll_s"].is<long>() || item["poll_s"].as<long>() < 0 || item["poll_s"].as<long>() > kMaxPollS) {
        err = where + ".poll_s must be a number of seconds, 0.." + String(static_cast<unsigned>(kMaxPollS)) +
              " (0: only when the connection is opened; null: every cycle)";
        return false;
      }
      const long s = item["poll_s"].as<long>();
      if (s == 0 && !d.keepConnected) {
        err = where + ".poll_s 0 (no polling, only pushes) needs keep_connected: true";
        return false;
      }
      d.pollS = s == 0 ? kPollOnConnect : static_cast<uint16_t>(s);
    }
    if (!item["meters"].isNull()) {
      if (!item["meters"].is<JsonObjectConst>()) {
        err = where + ".meters must be an object";
        return false;
      }
      struct Slot { const char* key; int16_t* target; };
      const Slot slots[] = {{"power", &d.dpPower}, {"current", &d.dpCurrent}, {"voltage", &d.dpVoltage}, {"energy", &d.dpEnergy}};
      for (const Slot& slot : slots) {
        JsonVariantConst v = item["meters"][slot.key];
        if (v.isNull()) continue;
        if (!v.is<long>() || v.as<long>() < 0 || v.as<long>() > 255) {
          err = where + ".meters." + slot.key + " must be an integer 0..255 (0 = none)";
          return false;
        }
        *slot.target = static_cast<int16_t>(v.as<long>());
      }
    }
    list.push_back(d);
  }
  out = list;
  return true;
}

// {"power":..,"current":..,"voltage":..,"energy":..}: null = the standard number is used.
void writeMeters(JsonObject m, const Device& d) {
  const struct { const char* key; int16_t v; } slots[] = {
      {"power", d.dpPower}, {"current", d.dpCurrent}, {"voltage", d.dpVoltage}, {"energy", d.dpEnergy}};
  for (const auto& slot : slots) {
    if (slot.v < 0) {
      m[slot.key] = nullptr;
    } else {
      m[slot.key] = slot.v;
    }
  }
}

// The file as it is stored on the card (with the keys).
String fileText(const std::vector<Device>& list) {
  JsonDocument doc(PsramAllocator::instance());
  doc["version"] = 1;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (const Device& d : list) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = d.name;
    o["ip"] = d.ip;
    o["key"] = d.key;
    o["version"] = Tuya::versionName(d.version);
    o["dp"] = d.dp;
    if (d.ignore) o["ignore"] = true;
    if (d.keepConnected) o["keep_connected"] = true;
    if (!d.updatedps) o["updatedps"] = false;
    if (d.pollSField() >= 0) o["poll_s"] = d.pollSField();
    if (d.dpPower >= 0 || d.dpCurrent >= 0 || d.dpVoltage >= 0 || d.dpEnergy >= 0) {
      JsonObject m = o["meters"].to<JsonObject>();
      if (d.dpPower >= 0) m["power"] = d.dpPower;
      if (d.dpCurrent >= 0) m["current"] = d.dpCurrent;
      if (d.dpVoltage >= 0) m["voltage"] = d.dpVoltage;
      if (d.dpEnergy >= 0) m["energy"] = d.dpEnergy;
    }
  }
  String out;
  serializeJsonPretty(doc, out);
  out += "\n";
  return out;
}

// Makes `list` the active list. A device that is gone or was edited (ip, key, version, dp, ignore)
// starts over with an unknown state and an empty history.
void applyList(const std::vector<Device>& list) {
  Lock lock;
  auto unchanged = [&](const String& name) {
    for (const Device& n : list) {
      if (n.name != name) continue;
      for (const Device& o : gList) {
        if (o.name == n.name && o.ip == n.ip && o.key == n.key && o.version == n.version && o.dp == n.dp && o.ignore == n.ignore) return true;
      }
    }
    return false;
  };
  for (auto it = gStates.begin(); it != gStates.end();) it = unchanged(it->first) ? std::next(it) : gStates.erase(it);
  for (auto it = gHistory.begin(); it != gHistory.end();) it = unchanged(it->first) ? std::next(it) : gHistory.erase(it);
  gList = list;
}

}  // namespace

void load() {
  { Lock init; }  // creates the mutex
  if (!gSaveMux) gSaveMux = xSemaphoreCreateMutex();

  LoadInfo info;
  info.sd = Storage::sdMounted();
  std::vector<Device> list;
  bool replace = false;
  // -1 also means "could not open": a card that has just come up can fail the first look (see Config::load).
  long size = info.sd ? Storage::sdSize(kPath) : -1;
  for (int i = 0; i < 2 && info.sd && size < 0; ++i) {
    delay(200);
    size = Storage::sdSize(kPath);
  }

  if (!info.sd) {
    info.error = "no SD card: devices are stored only on the card";
    LOGW(TAG, "%s", info.error.c_str());
    // The list in memory stays: the card may only be remounting, and wiping the devices would stop the monitor and
    // fail every scenario step. At start the list is empty anyway.
  } else if (size < 0) {
    // A save removes the old file before renaming the new one: a power cut in between leaves only
    // the complete temporary file. Never replace that with an empty template.
    String tmp, tmpErr;
    std::vector<Device> tmpList;
    if (Storage::sdReadAll(kTmp, tmp, kMaxFileBytes) && parse(tmp, tmpList, tmpErr, nullptr) && Storage::sdReplace(kTmp, kPath)) {
      list = tmpList;
      replace = true;
      info.fileFound = true;
      LOGW(TAG, "promoted %s to %s after an interrupted save", kTmp, kPath);
    } else {
      // sdCreate never overwrites: if the file is there after all (the card hid it), it and its keys stay.
      static const char kTemplate[] = "{\n  \"version\": 1,\n  \"devices\": []\n}\n";
      if (Storage::sdCreate(kPath, kTmp, String(kTemplate))) {
        replace = true;
        info.templateCreated = true;
        LOGW(TAG, "no %s, wrote an empty template: add the Tuya devices on the Devices page", kPath);
      } else {
        info.error = String("cannot open ") + kPath + " and could not create it (card error?): the previous list stays";
        LOGE(TAG, "%s", info.error.c_str());
      }
    }
  } else {
    info.fileFound = true;
    String raw, err;
    if (!Storage::sdReadAll(kPath, raw, kMaxFileBytes)) {
      info.error = String("cannot read ") + kPath + " (missing, too large or card error)";
      LOGE(TAG, "%s", info.error.c_str());
    } else if (!parse(raw, list, err, nullptr)) {
      info.error = String(kPath) + ": " + err;
      LOGE(TAG, "%s (the previous list stays active)", info.error.c_str());
    } else {
      replace = true;
    }
  }

  if (replace) applyList(list);
  Lock lock;
  info.count = gList.size();
  gInfo = info;
  if (replace && info.error.isEmpty()) LOGI(TAG, "%u device(s) loaded from %s", static_cast<unsigned>(info.count), kPath);
}

String configJson() {
  JsonDocument doc(PsramAllocator::instance());
  doc["max"] = kMaxDevices;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (const Device& d : list()) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = d.name;
    o["ip"] = d.ip;
    o["version"] = Tuya::versionName(d.version);
    o["dp"] = d.dp;
    o["key_set"] = true;  // the key is never sent
    o["ignore"] = d.ignore;
    o["keep_connected"] = d.keepConnected;
    o["updatedps"] = d.updatedps;
    if (d.pollSField() >= 0) {
      o["poll_s"] = d.pollSField();
    } else {
      o["poll_s"] = nullptr;  // every monitor cycle
    }
    writeMeters(o["meters"].to<JsonObject>(), d);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

SaveResult save(const String& json) {
  SaveResult r;
  if (!gSaveMux) {
    r.error = "devices are not initialised";
    return r;
  }
  SaveLock saveLock;  // concurrent saves are serialised; readers are not blocked by the SD write
  if (!Storage::sdMounted()) {
    r.noSd = true;
    r.error = "no SD card: the device list is stored only on the card";
    return r;
  }

  std::vector<Device> next;
  const std::vector<Device> current = list();
  if (!parse(json, next, r.error, &current)) {
    r.invalid = true;
    return r;
  }

  // One generation of backup: the keys are hard to recover, a wrong edit must not destroy them.
  String old;
  if (Storage::sdReadAll(kPath, old, kMaxFileBytes) && !old.isEmpty()) {
    if (!Storage::sdWriteAtomic(kBak, kBakTmp, old)) LOGW(TAG, "could not write the backup %s", kBak);
  }
  if (!Storage::sdWriteAtomic(kPath, kTmp, fileText(next))) {
    r.error = "writing to the SD card failed, the device list is unchanged";
    LOGE(TAG, "%s", r.error.c_str());
    return r;
  }

  applyList(next);
  {
    Lock lock;
    gInfo.sd = true;
    gInfo.fileFound = true;
    gInfo.error = "";
    gInfo.count = gList.size();
  }
  r.ok = true;
  r.count = next.size();
  LOGI(TAG, "device list saved from the web page: %u device(s)", static_cast<unsigned>(r.count));
  return r;
}

LoadInfo info() {
  Lock lock;
  return gInfo;
}

std::vector<Device> list() {
  Lock lock;
  return gList;
}

bool find(const String& name, Device& out) {
  Lock lock;
  for (const Device& d : gList) {
    if (d.name == name) {
      out = d;
      return true;
    }
  }
  return false;
}

bool state(const String& name, State& out) {
  Lock lock;
  for (const Device& d : gList) {
    if (d.name != name) continue;
    auto it = gStates.find(name);
    out = it == gStates.end() ? State() : it->second;
    return true;
  }
  return false;
}

uint16_t pollIntervalSec() {
  Lock lock;
  return gIntervalSec;
}

void monitorCycle(uint32_t& lastMs, uint32_t& maxMs) {
  lastMs = gLastCycleMs;
  maxMs = gMaxCycleMs;
}

void limits(JsonObject out) {
  const uint32_t interval = pollIntervalSec();
  out["max"] = kMaxDevices;
  out["max_file_bytes"] = kMaxFileBytes;
  out["history_capacity"] = kHistoryCapacity;
  out["history_span_s"] = static_cast<uint32_t>(kHistoryCapacity) * interval;  // at the current polling interval
  out["poll_interval_s"] = interval;
  out["poll_s_max"] = kMaxPollS;
}

namespace {

// The boolean at `dp` in a {"dps":{...}} JSON text: 1 true, 0 false, -1 absent or not a boolean.
int switchValue(const String& dpsJson, uint16_t dp) {
  if (dpsJson.isEmpty()) return -1;
  JsonDocument doc;
  if (deserializeJson(doc, dpsJson)) return -1;
  JsonVariantConst v = doc[String(dp)];
  return v.is<bool>() ? (v.as<bool>() ? 1 : 0) : -1;
}

// The switch of a device went from `was` to `now` (1 on, 0 off, -1 unknown): a change for the trigger "relay" when both
// are known and differ and the device was online before (see State::onSeq). The caller holds the lock.
void noteSwitch(State& st, bool wasOnline, int was, int now) {
  if (!wasOnline || was < 0 || now < 0 || was == now) return;
  if (++gRelaySeq == 0) ++gRelaySeq;  // 0 means "none"
  (now ? st.onSeq : st.offSeq) = gRelaySeq;
}

// The number in a {"dps":{...}} JSON at data point `dp`, INT32_MIN if absent or not a number.
int32_t numberAt(JsonVariantConst dps, int dp) {
  if (dp <= 0) return INT32_MIN;
  JsonVariantConst v = dps[String(dp)];
  return v.is<long>() ? static_cast<int32_t>(v.as<long>()) : INT32_MIN;
}

// Appends one sample to the history of `d`. The caller holds the lock and has checked that `d` is in the list;
// `dps` is the parsed data points of the answer (null for a failed query).
void appendSample(const Device& d, bool ok, JsonVariantConst dps) {
  Sample s;
  s.t = uptimeS();
  if (ok && !dps.isNull()) {
    s.flags |= Sample::kOk;
    const int16_t standard[4] = {19, 18, 20, 17};  // power, current, voltage, energy
    const int16_t chosen[4] = {d.dpPower, d.dpCurrent, d.dpVoltage, d.dpEnergy};
    for (size_t i = 0; i < 4; ++i) s.v[i] = numberAt(dps, chosen[i] < 0 ? standard[i] : chosen[i]);
    JsonVariantConst sw = dps[String(d.dp)];
    if (sw.is<bool>()) s.flags |= sw.as<bool>() ? Sample::kRelayKnown | Sample::kRelayOn : Sample::kRelayKnown;
  }
  History& h = gHistory[d.name];
  if (h.buf.capacity() < kHistoryCapacity) h.buf.reserve(kHistoryCapacity);
  if (h.buf.size() < kHistoryCapacity) {
    h.buf.push_back(s);
  } else {
    h.buf[h.head] = s;
    h.head = (h.head + 1) % kHistoryCapacity;
  }
}

// Stores the outcome of one status query and logs what changed. Called by the poller (once per
// cycle, after its retries) and by the observer (every user query). `target` and `dp` are what the query was sent
// with. Returns 1 if the device is online afterwards (see kOfflineAfterFails), 0 if not, and -1 if the answer was
// dropped: the device was removed or edited (ip, key, version, switch dp) while the query was running, so the answer
// belongs to a device that no longer exists under this name.
int applyQuery(const String& name, const Tuya::Target& target, uint16_t dp, const Tuya::Result& r) {
  String dps;
  JsonDocument dpsDoc;  // the data points of the answer
  if (r.ok) {
    JsonDocument payload;
    if (!deserializeJson(payload, r.json)) {
      JsonVariantConst v = payload["dps"];
      if (v.isNull()) v = payload["data"]["dps"];
      if (!v.isNull()) {
        serializeJson(v, dps);
        deserializeJson(dpsDoc, dps);
      }
    }
  }

  State before, after;
  int was = -1, now = -1;  // the switch before and after
  {
    Lock lock;
    const Device* dev = nullptr;
    for (const Device& x : gList) {
      if (x.name == name) dev = &x;
    }
    if (!dev || dev->ip != target.ip || dev->key != target.key || dev->version != target.version || dev->dp != dp) {
      return -1;
    }
    State& st = gStates[name];
    before = st;
    st.known = true;
    st.lastTryMs = millis();
    if (r.ok) {
      st.online = true;
      st.fails = 0;
      st.lastOkMs = st.lastTryMs ? st.lastTryMs : 1;
      st.latencyMs = r.ms;
      st.error = "";
      st.detail = "";
      if (!dps.isEmpty()) st.dpsJson = dps;
      was = switchValue(before.dpsJson, dp);
      now = switchValue(st.dpsJson, dp);
      noteSwitch(st, before.known && before.online, was, now);
    } else {
      ++st.fails;
      // A device that was online needs kOfflineAfterFails failures in a row; any other keeps being offline.
      st.online = before.known && before.online && st.fails < kOfflineAfterFails;
      st.error = r.error;
      st.detail = r.detail;
    }
    after = st;
    appendSample(*dev, r.ok, dpsDoc.as<JsonVariantConst>());  // same lock: the device cannot vanish in between
  }

  if (r.ok && (!before.known || !before.online)) {
    LOGI(TAG, "[%s] online (answered in %lu ms)", name.c_str(), static_cast<unsigned long>(r.ms));
  } else if (!r.ok && !after.online && (!before.known || before.online)) {
    LOGW(TAG, "[%s] OFFLINE: %s: %s", name.c_str(), r.error.c_str(), r.detail.c_str());
  } else if (!r.ok && after.online) {
    LOGI(TAG, "[%s] did not answer (%u of %u before it counts as offline): %s", name.c_str(), static_cast<unsigned>(after.fails),
         static_cast<unsigned>(kOfflineAfterFails), r.error.c_str());
  }
  if (r.ok) {
    if (now >= 0 && was != now) {
      LOGI(TAG, "[%s] switch (dp %u) is now %s%s", name.c_str(), static_cast<unsigned>(dp), now ? "ON" : "OFF",
           was < 0 ? " (first reading)" : !before.online ? " (first answer after being offline)" : "");
    }
  }
  return after.online ? 1 : 0;
}

// A device with a live kept connection that is not queried this cycle (poll_s): its cached state (the last query with
// the pushes since merged in) becomes a sample of the history, as a poll's answer would. Nothing for a device that was removed or edited meanwhile.
void recordCached(const String& name, const Tuya::Target& target, uint16_t dp) {
  Lock lock;
  const Device* dev = nullptr;
  for (const Device& x : gList) {
    if (x.name == name) dev = &x;
  }
  if (!dev || dev->ip != target.ip || dev->key != target.key || dev->version != target.version || dev->dp != dp) return;
  auto it = gStates.find(name);
  if (it == gStates.end() || !it->second.known) return;
  JsonDocument dps;
  const bool ok = it->second.online && !it->second.dpsJson.isEmpty() && !deserializeJson(dps, it->second.dpsJson);
  appendSample(*dev, ok, dps.as<JsonVariantConst>());
}

// One TCP connect to the device's Tuya port (no session): tells "the device is up but turns the session away" from
// "it does not answer at all". Takes up to 1.5 s, so only the monitor task calls it (never a worker or a web handler).
String probeText(const Device& d) {
  IPAddress ip;
  if (!ip.fromString(d.ip)) return "bad IP";
  WiFiClient c;
  const uint32_t t0 = millis();
  const bool ok = c.connect(ip, 6668, 1500);
  const uint32_t ms = millis() - t0;
  c.stop();
  String out = String("TCP ") + d.ip + ":6668 ";
  if (ok) {
    out += "accepted in " + String(static_cast<unsigned long>(ms)) + " ms (the device is up: the failure is in the session, or it was a one-off)";
  } else if (ms < 400) {
    out += "refused in " + String(static_cast<unsigned long>(ms)) + " ms (the device answers but has no free connection, or is restarting)";
  } else {
    out += "no answer in " + String(static_cast<unsigned long>(ms)) + " ms (radio asleep, out of range or powered off)";
  }
  return out;
}

struct MonitorParams {
  uint16_t intervalSec;
  uint16_t timeoutMs;
  uint8_t attempts;
};

void monitorTask(void* arg) {
  const MonitorParams p = *static_cast<MonitorParams*>(arg);
  delete static_cast<MonitorParams*>(arg);

  // A device that did not answer a whole poll is "offline" here: it gets one attempt instead of
  // p.attempts and is polled only every kOfflineEvery-th cycle, so it cannot hold up the others.
  // It is pulled back to normal polling as soon as it answers (or the user presses "refresh").
  constexpr uint8_t kOfflineEvery = 2;
  // Whether a device is "offline" is read from its state (not remembered here), so a device that was removed, edited
  // or added again starts as a new one: polled in full at once, with no skipping left over from its previous life.
  std::map<String, uint8_t> cyclesToSkip;
  // A device with updatedps that does not take UPDATEDPS gets plain queries for this many cycles before it is tried again.
  constexpr uint16_t kRefreshRetryCycles = 20;
  std::map<String, uint16_t> noRefresh;
  // A device that stays offline: one line about why, at most this often (a failed poll is otherwise only a debug line,
  // so the log would say "OFFLINE" and nothing until the device is back).
  constexpr uint32_t kDiagEveryMs = 60000;
  std::map<String, uint32_t> diagAt;
  // millis() of the last query of each device that got an answer or a failure (see poll_s).
  std::map<String, uint32_t> queriedAt;
  const uint32_t period = static_cast<uint32_t>(p.intervalSec) * 1000UL;
  bool slow = false;  // the last cycle took longer than the interval

  vTaskDelay(pdMS_TO_TICKS(5000));  // let Wi-Fi come up
  for (;;) {
    const uint32_t cycleStart = millis();
    if (WiFi.status() == WL_CONNECTED) {
      const std::vector<Device> devices = list();
      std::vector<String> kept;  // the polled devices with keep_connected: the kept connections of any others are closed
      for (const Device& d : devices) {
        if (d.target().keep && !d.ignore) kept.push_back(d.ip);
      }
      Tuya::keepOnly(kept);
      for (auto it = cyclesToSkip.begin(); it != cyclesToSkip.end();) {  // forget the devices that are gone
        bool present = false;
        for (const Device& d : devices) present = present || d.name == it->first;
        it = present ? std::next(it) : cyclesToSkip.erase(it);
      }
      for (auto it = noRefresh.begin(); it != noRefresh.end();) {
        bool present = false;
        for (const Device& d : devices) present = present || d.name == it->first;
        it = present ? std::next(it) : noRefresh.erase(it);
      }
      for (auto it = diagAt.begin(); it != diagAt.end();) {
        bool present = false;
        for (const Device& d : devices) present = present || d.name == it->first;
        it = present ? std::next(it) : diagAt.erase(it);
      }
      for (auto it = queriedAt.begin(); it != queriedAt.end();) {
        bool present = false;
        for (const Device& d : devices) present = present || d.name == it->first;
        it = present ? std::next(it) : queriedAt.erase(it);
      }
      for (const Device& listed : devices) {
        // The list may have changed since this cycle began (a device removed, edited, ignored): work with the current one.
        Device d;
        if (!find(listed.name, d) || d.ignore) {  // an ignored device is not polled at all; un-ignored later it starts as new
          cyclesToSkip.erase(listed.name);
          continue;
        }
        State known;
        const bool wasOffline = state(d.name, known) && known.known && !known.online;
        if (!wasOffline) {
          cyclesToSkip.erase(d.name);
        } else if (cyclesToSkip[d.name] > 0) {
          --cyclesToSkip[d.name];
          continue;
        }
        const uint8_t attempts = wasOffline ? 1 : p.attempts;
        // A command is queued or running for this device: it will refresh the cached state itself, and a poll
        // would only collide with it. Leave the device alone this cycle.
        if (Tuya::commandPending(d.ip)) continue;

        // poll_s: a query only once every poll_s, whether the device pushes or not (half a cycle of slack, so that 60 s
        // at a 15 s interval is every 4th cycle and not, after a little delay, the 5th). Between two queries a device with
        // a live kept connection gets its cached state as a history sample (what it pushes goes into that state); one
        // without gets nothing: there is no new information. A lost kept connection is opened again by a query now, and
        // a device counted offline is queried although its connection lives (a command may have opened it). poll_s 0
        // (kPollOnConnect): only those queries.
        auto q = queriedAt.find(d.name);
        const bool due = d.pollS != kPollOnConnect &&
                         (d.pollS <= p.intervalSec || q == queriedAt.end() || millis() - q->second + period / 2 >= d.pollS * 1000UL);
        if (!due) {
          if (Tuya::kept(d.ip) && !wasOffline) {
            recordCached(d.name, d.target(), d.dp);
            continue;
          }
          if (!d.target().keep) continue;
        }

        // updatedps (the device's own setting, not whether a power rule waits for it): UPDATEDPS + status (fresh
        // readings) instead of the plain query. If it does not take that command: plain queries for a while.
        const String meters = d.updatedps ? meterDps(d) : String();
        bool useRefresh = !meters.isEmpty();
        auto skipIt = noRefresh.find(d.name);
        if (skipIt != noRefresh.end()) {
          useRefresh = false;
          if (--skipIt->second == 0) noRefresh.erase(skipIt);
        }

        Tuya::Result last;
        // UPDATEDPS failed in a way that can mean "the device does not know it". "closed" is also what a device with all its
        // connection slots taken answers, so the device is marked only if the plain query right after it does get through.
        bool refreshRejected = false;
        for (int attempt = 0; attempt < attempts; ++attempt) {
          Tuya::RefreshOpts ro;
          ro.dpIds = meters;
          ro.settleMs = 500;
          const uint32_t timeoutMs = useRefresh ? std::max<uint32_t>(p.timeoutMs, 3000) : p.timeoutMs;
          std::shared_ptr<Tuya::Job> job = useRefresh ? Tuya::submit(Tuya::Op::Refresh, d.name, d.target(), d.dp, timeoutMs, true, -1, &ro)
                                                      : Tuya::submit(Tuya::Op::Query, d.name, d.target(), d.dp, timeoutMs, true);
          if (!job) {  // the poll list is full: poll this device next cycle
            last.error = "busy";
            last.detail = "the device queue was full";
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
          }
          // An operation is its connect (<= kConnectMaxMs) plus the timeout; over a kept connection a timed-out one is tried
          // once more on a new one (connect, timeout again).
          const uint32_t waitUntil = millis() + (d.target().keep ? 2 * timeoutMs : timeoutMs) + Tuya::kConnectMaxMs + 5000;
          while (!job->done.load(std::memory_order_acquire) && static_cast<int32_t>(waitUntil - millis()) > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
          }
          if (!job->done.load(std::memory_order_acquire)) {
            job->cancelled.store(true);  // still queued: withdrawn, so it does not pile up behind the next attempt
            last.error = "busy";  // not started or still running: no information about the device, not a failure
            last.detail = "the query did not finish in time";
            continue;
          }
          last = job->result;
          if (useRefresh && !last.ok && (last.error == "closed" || last.error == "protocol" || last.error == "device")) {
            refreshRejected = true;
            useRefresh = false;
            --attempt;  // the same attempt again, with the plain query
            continue;
          }
          if (last.ok || last.error == "busy") break;  // "busy": a command had the device; try next cycle
          // A kept connection that did not answer was already tried once more on a new one: another attempt would only
          // hold the single monitor (connect + timeout again) while the other devices wait.
          if (last.retried) break;
          if (attempt + 1 < attempts) vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (refreshRejected && last.ok) {
          LOGI(TAG, "[%s] does not take UPDATEDPS: plain status queries for the next %u cycles", d.name.c_str(),
               static_cast<unsigned>(kRefreshRetryCycles));
          noRefresh[d.name] = kRefreshRetryCycles;
        }
        if (last.error == "busy") continue;
        {
          Device now;
          if (!find(d.name, now) || now.ignore) continue;  // ignored or removed while the poll ran: not for the state
        }
        // Only a device that is really offline (see kOfflineAfterFails) gets the cheap treatment; one that missed a
        // single poll is polled in full again next cycle, to confirm quickly.
        const int online = applyQuery(d.name, d.target(), d.dp, last);
        if (online < 0) continue;  // the device was removed or edited while the poll ran: the answer was dropped
        queriedAt[d.name] = millis();
        cyclesToSkip[d.name] = online == 1 ? 0 : kOfflineEvery - 1;
        if (online == 1) {
          diagAt.erase(d.name);
        } else if (!last.ok) {
          const uint32_t nowMs = millis();
          auto at = diagAt.find(d.name);
          if ((at == diagAt.end() || nowMs - at->second >= kDiagEveryMs) && !Tuya::commandPending(d.ip)) {
            diagAt[d.name] = nowMs;
            State st;
            state(d.name, st);
            LOGI(TAG, "[%s] still failing: %s (connect %lu ms, total %lu ms), %u failed in a row; %s; Wi-Fi %d dBm", d.name.c_str(),
                 last.error.c_str(), static_cast<unsigned long>(last.connectMs), static_cast<unsigned long>(last.ms),
                 static_cast<unsigned>(st.fails), probeText(d).c_str(), WiFi.RSSI());
          }
        }
      }
    }
    const uint32_t spent = millis() - cycleStart;
    gLastCycleMs = spent;
    if (spent > gMaxCycleMs) gMaxCycleMs = spent;
    if (spent > period && !slow) {  // one line per slow streak, not one per cycle
      LOGW(TAG, "monitor cycle took %lu ms, longer than the %u s interval: the devices are polled less often",
           static_cast<unsigned long>(spent), static_cast<unsigned>(p.intervalSec));
    } else if (spent <= period && slow) {
      LOGI(TAG, "monitor cycle back within the interval (%lu ms)", static_cast<unsigned long>(spent));
    }
    slow = spent > period;
    vTaskDelay(pdMS_TO_TICKS(spent < period ? period - spent : 1000));
  }
}

}  // namespace

uint32_t relaySeq() {
  Lock lock;
  return gRelaySeq;
}

int switchState(const String& name) {
  Device d;
  State st;
  if (!find(name, d) || !state(name, st) || !st.known || !st.online) return -1;
  return switchValue(st.dpsJson, d.dp);
}

PowerCheck checkPower(const String& name, bool below, uint32_t watts, uint32_t forS, uint32_t warmupS) {
  PowerCheck pc;
  Device d;
  if (!find(name, d)) {
    pc.reason = "no-device";
    return pc;
  }
  if (d.ignore) {
    pc.reason = "ignored";
    return pc;
  }
  if (d.dpPower == 0) {
    pc.reason = "no-power-dp";
    return pc;
  }
  std::vector<Sample> s;  // oldest first
  {
    Lock lock;
    auto it = gHistory.find(name);
    if (it != gHistory.end()) {
      const History& h = it->second;
      const size_t n = h.buf.size();
      s.reserve(n);
      const size_t start = n < kHistoryCapacity ? 0 : h.head;
      for (size_t i = 0; i < n; ++i) s.push_back(h.buf[(start + i) % n]);
    }
  }
  const PowerRule::Result r = PowerRule::evaluate(s, kHistoryCapacity, uptimeS(), pollIntervalSec(), below, watts, forS, warmupS);
  pc.met = r.met;
  pc.reason = r.reason;
  pc.lastPower = r.lastPower;
  pc.runS = r.runS;
  return pc;
}

bool historyJson(const String& name, size_t limit, String& out) {
  Device d;
  if (!find(name, d)) return false;
  std::vector<Sample> samples;
  {
    Lock lock;
    auto it = gHistory.find(name);
    if (it != gHistory.end()) {
      const History& h = it->second;
      const size_t n = h.buf.size();
      samples.reserve(n);
      const size_t start = n < kHistoryCapacity ? 0 : h.head;  // the oldest
      for (size_t i = 0; i < n; ++i) samples.push_back(h.buf[(start + i) % n]);
    }
  }
  if (limit > 0 && samples.size() > limit) samples.erase(samples.begin(), samples.end() - limit);

  const uint32_t nowUp = uptimeS();
  const time_t nowEpoch = time(nullptr);
  const bool timeValid = nowEpoch > 1700000000;
  JsonDocument doc(PsramAllocator::instance());
  doc["name"] = name;
  doc["capacity"] = kHistoryCapacity;
  doc["poll_interval_s"] = pollIntervalSec();
  doc["time_valid"] = timeValid;
  JsonArray arr = doc["samples"].to<JsonArray>();
  const char* keys[4] = {"power", "current", "voltage", "energy"};
  for (const Sample& s : samples) {
    JsonObject o = arr.add<JsonObject>();
    const uint32_t age = nowUp - s.t;
    o["age_s"] = age;
    if (timeValid) {
      o["time"] = static_cast<uint32_t>(nowEpoch) - age;
    } else {
      o["time"] = nullptr;
    }
    o["ok"] = (s.flags & Sample::kOk) != 0;
    if (s.flags & Sample::kRelayKnown) {
      o["on"] = (s.flags & Sample::kRelayOn) != 0;
    } else {
      o["on"] = nullptr;
    }
    for (size_t i = 0; i < 4; ++i) {
      if (s.v[i] == INT32_MIN) {
        o[keys[i]] = nullptr;
      } else {
        o[keys[i]] = s.v[i];
      }
    }
  }
  serializeJson(doc, out);
  return true;
}

String meterDps(const Device& d) {
  const int16_t standard[] = {18, 19, 20, 17};  // current, power, voltage, energy
  const int16_t chosen[] = {d.dpCurrent, d.dpPower, d.dpVoltage, d.dpEnergy};
  String out;
  int16_t seen[4];
  size_t n = 0;
  for (size_t i = 0; i < 4; ++i) {
    const int16_t v = chosen[i] < 0 ? standard[i] : chosen[i];
    if (v == 0) continue;
    bool dup = false;
    for (size_t j = 0; j < n; ++j) dup = dup || seen[j] == v;
    if (dup) continue;
    seen[n++] = v;
    if (!out.isEmpty()) out += ',';
    out += String(v);
  }
  return out;
}

void onJobDone(const Tuya::Job& job) {
  // Polls are applied by the poller after its retries; control commands do not tell the state.
  if (job.background || (job.op != Tuya::Op::Query && job.op != Tuya::Op::Refresh)) return;
  if (job.result.error == "busy" || job.result.error == "cancelled") return;  // the device was never asked: nothing to learn from it
  applyQuery(job.device, job.target, job.dp, job.result);
}

void onPush(const String& name, const Tuya::Target& target, const String& json) {
  JsonDocument payload;
  if (deserializeJson(payload, json)) return;
  JsonVariantConst pushed = payload["dps"];
  if (pushed.isNull()) pushed = payload["data"]["dps"];
  if (!pushed.is<JsonObjectConst>()) return;
  int was = -1, now = -1;
  uint16_t dp = 0;
  {
    Lock lock;
    const Device* dev = nullptr;
    for (const Device& x : gList) {
      if (x.name == name) dev = &x;
    }
    if (!dev || dev->ignore || dev->ip != target.ip || dev->key != target.key || dev->version != target.version) return;
    dp = dev->dp;
    State& st = gStates[name];
    JsonDocument merged;
    if (st.dpsJson.isEmpty() || deserializeJson(merged, st.dpsJson) || !merged.is<JsonObject>()) merged.to<JsonObject>();
    const String key = String(dp);
    if (merged[key].is<bool>()) was = merged[key].as<bool>() ? 1 : 0;
    for (JsonPairConst kv : pushed.as<JsonObjectConst>()) merged[kv.key()] = kv.value();
    if (merged[key].is<bool>()) now = merged[key].as<bool>() ? 1 : 0;
    noteSwitch(st, st.known && st.online, was, now);
    st.dpsJson = "";
    serializeJson(merged, st.dpsJson);
    st.lastPushMs = millis() ? millis() : 1;
    ++st.pushes;
  }
  if (now >= 0 && was != now) {
    LOGI(TAG, "[%s] switch (dp %u) is now %s (pushed)", name.c_str(), static_cast<unsigned>(dp), now ? "ON" : "OFF");
  }
}

void startMonitor(uint16_t intervalSec, uint16_t timeoutMs, uint8_t attempts) {
  {
    Lock lock;
    gIntervalSec = intervalSec;
  }
  auto* p = new MonitorParams{intervalSec, timeoutMs, static_cast<uint8_t>(attempts ? attempts : 1)};
  xTaskCreate(monitorTask, "devmon", 6144, p, 1, nullptr);
}

}  // namespace Devices
