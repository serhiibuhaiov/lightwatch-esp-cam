#include "config.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage.h"

namespace Config {
namespace {

const char* TAG = "config";
const char* kSdPath = "/config.json";
const char* kSdTmp = "/config.tmp";
const char* kSdBad = "/config.bad";  // an unreadable config.json is parked here, never overwritten
const char* kFlashPath = "/config.json";

SemaphoreHandle_t gMux = nullptr;      // guards the state below; held for copies, never for I/O
SemaphoreHandle_t gSaveMux = nullptr;  // serialises saves; held across the SD write

// Two views of the configuration:
//   gActive - what the running firmware was started with (fixed by load()).
//   gSaved  - what is stored on the card/flash now (updated by saveFromJson()).
// They differ between a PUT and the next reboot.
AppConfig gActive;
AppConfig gSaved;
String gActiveText;      // compact JSON of gActive, to detect whether a save changed anything
bool gPending = false;   // gSaved != gActive
Source gSource = Source::Defaults;

struct Lock {
  Lock() {
    if (!gMux) gMux = xSemaphoreCreateMutex();
    xSemaphoreTake(gMux, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(gMux); }
};

struct SaveLock {
  SaveLock() {
    if (!gSaveMux) gSaveMux = xSemaphoreCreateMutex();
    xSemaphoreTake(gSaveMux, portMAX_DELAY);
  }
  ~SaveLock() { xSemaphoreGive(gSaveMux); }
};

// ---- Field readers: absent -> keep default; wrong type or range -> error ------

template <typename T>
bool readNum(JsonVariantConst v, const char* name, T& out, long minV, long maxV, String& err) {
  if (v.isNull()) return true;
  if (!v.is<long>()) {
    err = String(name) + " must be an integer";
    return false;
  }
  long x = v.as<long>();
  if (x < minV || x > maxV) {
    err = String(name) + " must be in " + String(minV) + ".." + String(maxV);
    return false;
  }
  out = static_cast<T>(x);
  return true;
}

bool readStr(JsonVariantConst v, const char* name, String& out, size_t maxLen, String& err) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) {
    err = String(name) + " must be a string";
    return false;
  }
  const char* s = v.as<const char*>();
  if (strlen(s) > maxLen) {
    err = String(name) + " is longer than " + String(static_cast<unsigned>(maxLen));
    return false;
  }
  out = s;
  return true;
}

bool validHostname(const String& s) {
  if (s.isEmpty() || s.length() > 32) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return s[0] != '-' && s[s.length() - 1] != '-';  // RFC 952: no leading or trailing hyphen
}

bool validLevel(const String& s) {
  return s.equalsIgnoreCase("debug") || s.equalsIgnoreCase("info") || s.equalsIgnoreCase("warn") ||
         s.equalsIgnoreCase("error");
}

// Parses and validates. On success fills `out`; on failure `out` is untouched.
// `keep` supplies the secrets to retain when the document leaves them empty.
bool parse(const String& json, AppConfig& out, String& err, const AppConfig* keep) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) {
    err = String("invalid JSON: ") + e.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    err = "root must be an object";
    return false;
  }

  AppConfig c;
  JsonVariantConst r = doc.as<JsonVariantConst>();

  if (!readNum(r["version"], "version", c.version, 1, 1000, err)) return false;

  // device
  JsonVariantConst d = r["device"];
  if (!readStr(d["name"], "device.name", c.device.name, 32, err)) return false;
  if (!validHostname(c.device.name)) {
    err = "device.name must be 1..32 chars of A-Z a-z 0-9 - (no leading or trailing -)";
    return false;
  }
  if (!readStr(d["web_user"], "device.web_user", c.device.webUser, 32, err)) return false;
  if (c.device.webUser.isEmpty()) {
    err = "device.web_user must not be empty";
    return false;
  }
  if (!readStr(d["web_password"], "device.web_password", c.device.webPassword, 64, err)) return false;
  if (c.device.webPassword.isEmpty() && keep) c.device.webPassword = keep->device.webPassword;

  // wifi
  JsonVariantConst w = r["wifi"];
  if (!readStr(w["ssid"], "wifi.ssid", c.wifi.ssid, 32, err)) return false;
  if (!readStr(w["password"], "wifi.password", c.wifi.password, 64, err)) return false;
  if (c.wifi.password.isEmpty() && keep && c.wifi.ssid == keep->wifi.ssid) {
    c.wifi.password = keep->wifi.password;  // same network: an empty field means "unchanged"
  }
  if (!readNum(w["restart_after_min"], "wifi.restart_after_min", c.wifi.restartAfterMin, 0, 1440, err)) return false;

  // startup
  JsonVariantConst s = r["startup"];
  if (!readNum(s["scenario_delay_s"], "startup.scenario_delay_s", c.startup.scenarioDelaySec, 0, 3600, err)) return false;

  // monitor
  JsonVariantConst m = r["monitor"];
  if (!readNum(m["interval_s"], "monitor.interval_s", c.monitor.intervalSec, 1, 3600, err)) return false;
  if (!readNum(m["timeout_ms"], "monitor.timeout_ms", c.monitor.timeoutMs, 100, 60000, err)) return false;
  if (!readNum(m["retries"], "monitor.retries", c.monitor.retries, 0, 10, err)) return false;
  if (!readNum(m["grace_s"], "monitor.grace_s", c.monitor.graceSec, 0, 3600, err)) return false;

  // "devices" moved to /devices.json (SD only). An old config.json may still carry it: it is
  // ignored, and dropped at the next save. `keep == nullptr` is the load path, so PUTs do not warn.
  if (!keep && r["devices"].is<JsonArrayConst>() && r["devices"].size() > 0) {
    LOGW(TAG, "\"devices\" in config.json is ignored: Tuya devices now live in /devices.json on the SD card");
  }

  // log
  JsonVariantConst l = r["log"];
  if (!readNum(l["flush_s"], "log.flush_s", c.log.flushSec, 1, 3600, err)) return false;
  if (!readNum(l["max_file_kb"], "log.max_file_kb", c.log.maxFileKb, 16, 8192, err)) return false;
  if (!readNum(l["keep_files"], "log.keep_files", c.log.keepFiles, 1, 50, err)) return false;
  if (!readNum(l["quiet_s"], "log.quiet_s", c.log.quietSec, 0, 600, err)) return false;
  String level = Logger::levelName(c.log.minLevel);
  if (!readStr(l["level"], "log.level", level, 8, err)) return false;
  if (!validLevel(level)) {
    err = "log.level must be debug, info, warn or error";
    return false;
  }
  c.log.minLevel = Logger::levelFromString(level, c.log.minLevel);

  // time
  JsonVariantConst t = r["time"];
  if (!readStr(t["ntp"], "time.ntp", c.time.ntp, 64, err)) return false;
  if (!readStr(t["tz"], "time.tz", c.time.tz, 64, err)) return false;

  out = c;
  return true;
}

}  // namespace

String toJson(const AppConfig& c, bool includeSecrets, bool pretty) {
  JsonDocument doc;
  doc["version"] = c.version;

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["name"] = c.device.name;
  dev["web_user"] = c.device.webUser;
  dev["web_password"] = includeSecrets ? c.device.webPassword : String("");

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = c.wifi.ssid;
  wifi["password"] = includeSecrets ? c.wifi.password : String("");
  wifi["restart_after_min"] = c.wifi.restartAfterMin;

  doc["startup"]["scenario_delay_s"] = c.startup.scenarioDelaySec;

  JsonObject mon = doc["monitor"].to<JsonObject>();
  mon["interval_s"] = c.monitor.intervalSec;
  mon["timeout_ms"] = c.monitor.timeoutMs;
  mon["retries"] = c.monitor.retries;
  mon["grace_s"] = c.monitor.graceSec;

  JsonObject log = doc["log"].to<JsonObject>();
  log["flush_s"] = c.log.flushSec;
  log["max_file_kb"] = c.log.maxFileKb;
  log["keep_files"] = c.log.keepFiles;
  log["quiet_s"] = c.log.quietSec;
  log["level"] = Logger::levelName(c.log.minLevel);

  JsonObject tm = doc["time"].to<JsonObject>();
  tm["ntp"] = c.time.ntp;
  tm["tz"] = c.time.tz;

  String out;
  if (pretty) {
    serializeJsonPretty(doc, out);
  } else {
    serializeJson(doc, out);
  }
  return out;
}

Source load() {
  { Lock init; }  // makes sure the mutex exists

  AppConfig c;
  String raw, err;
  Source source = Source::Defaults;
  bool have = false;
  const bool sdUp = Storage::sdMounted();
  // config.json is there, small enough, and still cannot be read: that is the card, not the file. Nothing on the card is
  // touched then (no .bad, no restore from the flash copy over the user's file): this boot runs on the flash copy.
  bool cardTrouble = false;

  if (sdUp) {
    bool read = false;
    for (int i = 0; i < 3 && !read; ++i) {  // a card that has just come up can fail the first read
      read = Storage::sdReadAll(kSdPath, raw);
      if (!read && i < 2) delay(200);
    }
    const long onCard = Storage::sdSize(kSdPath);
    if (read && parse(raw, c, err, nullptr)) {
      have = true;
      source = Source::Sd;
      Storage::sdRemove(kSdTmp);  // leftover from an interrupted save
    } else if (!read && onCard >= 0 && onCard <= 16384) {
      cardTrouble = true;
      LOGE(TAG, "%s is on the card (%ld bytes) but cannot be read: the card is in trouble. The file is left alone, this boot uses the flash copy", kSdPath, onCard);
    } else {
      if (!err.isEmpty()) LOGW(TAG, "%s is invalid: %s", kSdPath, err.c_str());
      // Park the unusable file before anything is restored over it, so a typo made
      // while editing on a PC does not destroy the user's work.
      if (Storage::sdSize(kSdPath) >= 0) {
        if (Storage::sdReplace(kSdPath, kSdBad)) {
          LOGW(TAG, "moved the unusable %s to %s", kSdPath, kSdBad);
        } else {
          LOGE(TAG, "could not move the unusable %s aside", kSdPath);
        }
      }
      String tmp, tmpErr;
      if (Storage::sdReadAll(kSdTmp, tmp) && parse(tmp, c, tmpErr, nullptr)) {
        if (Storage::sdReplace(kSdTmp, kSdPath)) {
          have = true;
          raw = tmp;
          source = Source::SdRecovered;
          LOGW(TAG, "promoted %s to %s after an interrupted save", kSdTmp, kSdPath);
        }
      }
    }
  }

  if (!have) {
    String flash, flashErr;
    if (Storage::lfsRead(kFlashPath, flash) && parse(flash, c, flashErr, nullptr)) {
      have = true;
      raw = flash;
      source = Source::Flash;
      if (sdUp && !cardTrouble) {
        if (Storage::sdWriteAtomic(kSdPath, kSdTmp, flash)) {
          LOGW(TAG, "restored %s on the SD card from the flash copy", kSdPath);
        } else {
          LOGE(TAG, "could not restore %s on the SD card", kSdPath);
        }
      }
    }
  }

  if (!have) {
    c = AppConfig();
    source = Source::Defaults;
    if (sdUp && !cardTrouble) {
      String tpl = toJson(c, true);
      if (Storage::sdWriteAtomic(kSdPath, kSdTmp, tpl)) {
        LOGW(TAG, "no config found, wrote a template %s: fill in wifi and web_password", kSdPath);
      }
    }
  } else if (source == Source::Sd || source == Source::SdRecovered) {
    Storage::lfsWrite(kFlashPath, raw);  // keep the flash copy in sync (no-op if equal)
  }

  {
    Lock lock;
    gActive = c;
    gSaved = c;
    gActiveText = toJson(c, true, false);
    gPending = false;
    gSource = source;
  }
  return source;
}

Source loadedFrom() {
  Lock lock;
  return gSource;
}

const char* sourceName(Source s) {
  switch (s) {
    case Source::Sd: return "SD card";
    case Source::SdRecovered: return "SD card (recovered config.tmp)";
    case Source::Flash: return "flash copy";
    default: return "defaults";
  }
}

AppConfig snapshot() {
  Lock lock;
  return gActive;
}

AppConfig savedSnapshot() {
  Lock lock;
  return gSaved;
}

bool restartPending() {
  Lock lock;
  return gPending;
}

bool webCredentials(String& user, String& password) {
  Lock lock;
  user = gSaved.device.webUser;  // saved, not active: a new password works at once
  password = gSaved.device.webPassword;
  return !password.isEmpty();
}

SaveResult saveFromJson(const String& json) {
  SaveResult r;
  SaveLock save;  // concurrent PUTs are serialised; readers of the config are not blocked by the SD write

  const AppConfig current = savedSnapshot();
  AppConfig next;
  if (!parse(json, next, r.error, &current)) {
    r.invalid = true;
    return r;
  }
  String text = toJson(next, true);

  if (Storage::sdMounted()) {
    // SD is the source of truth: it must succeed. The flash copy is best effort.
    r.sd = Storage::sdWriteAtomic(kSdPath, kSdTmp, text);
    if (r.sd) r.flash = Storage::lfsWrite(kFlashPath, text);
    r.ok = r.sd;
    if (!r.ok) r.error = "writing to the SD card failed, config unchanged";
  } else {
    r.flash = Storage::lfsWrite(kFlashPath, text);
    r.ok = r.flash;
    if (!r.ok) r.error = "writing to flash failed, config unchanged";
  }

  if (r.ok) {
    const String nextText = toJson(next, true, false);
    {
      Lock lock;
      gSaved = next;
      gPending = nextText != gActiveText;
    }
    LOGI(TAG, "config saved (sd=%d flash=%d)", r.sd, r.flash);
  } else {
    LOGE(TAG, "config save failed: %s", r.error.c_str());
  }
  return r;
}

}  // namespace Config
