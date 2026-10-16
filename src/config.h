#pragma once
//
// Config: settings stored as /config.json on the SD card, with a copy in
// internal flash (LittleFS) so the card is not the single point of failure.
//
// Source order at boot:
//   1. /config.json on SD (valid)           -> used; stale /config.tmp removed
//   2. /config.tmp on SD (valid, newer)     -> promoted to /config.json
//   3. copy in LittleFS (valid)             -> used; written back to the SD
//   4. built-in defaults                    -> a template /config.json is created
//
// Thread safety: snapshot()/webCredentials() copy under a short mutex that is
// never held during I/O; saveFromJson() uses a separate mutex for the whole save,
// so concurrent PUTs are serialised without blocking readers (the web handlers).
//
// An unreadable /config.json is renamed to /config.bad before a fallback is
// restored over it.
//
#include <Arduino.h>

#include "logger.h"

struct AppConfig {
  int version = 1;

  struct {
    String name = "lightwatch";  // also the Wi-Fi hostname
    String webUser = "admin";
    String webPassword;          // empty = web API locked
  } device;

  struct {
    String ssid;
    String password;
    uint16_t restartAfterMin = 20;  // 0 = never restart on lost Wi-Fi
  } wifi;

  struct {
    uint16_t scenarioDelaySec = 45;  // reserved for the scenario task
  } startup;

  struct {                           // reserved for the polling task
    uint16_t intervalSec = 30;
    uint16_t timeoutMs = 3000;
    uint8_t retries = 3;
    uint16_t graceSec = 120;
  } monitor;

  // The Tuya devices are NOT here: they live in /devices.json on the SD card only (see devices.h).

  Logger::Settings log;

  struct {
    String ntp = "pool.ntp.org";
    String tz = "UTC0";  // POSIX TZ string; UTC keeps the log free of daylight-saving jumps
  } time;
};

namespace Config {

enum class Source { Sd, SdRecovered, Flash, Defaults };

// Call once from setup() after Storage::begin().
Source load();
Source loadedFrom();                 // what load() used
const char* sourceName(Source s);    // for logs and the status endpoint

// The ACTIVE configuration: what the firmware started with. Constant between
// reboots, so every task sees one consistent set of values.
AppConfig snapshot();

// The SAVED configuration (SD/flash): the active one plus whatever PUT changed
// since boot. It takes effect after a restart.
AppConfig savedSnapshot();

// True when the saved configuration differs from the active one.
bool restartPending();

// Copies the web credentials from the SAVED config, so a new password works at once; returns false when no password is configured.
bool webCredentials(String& user, String& password);

// includeSecrets=false blanks wifi.password and device.web_password.
String toJson(const AppConfig& cfg, bool includeSecrets, bool pretty = true);

struct SaveResult {
  bool ok = false;
  bool invalid = false;  // the document was rejected (HTTP 400), see error
  bool sd = false;       // written to the SD card
  bool flash = false;    // written to the flash copy
  String error;
};

// Validates json, then persists it. PUT semantics: a full replacement, absent
// fields get defaults. wifi.password / device.web_password that are missing or
// empty keep their current value.
SaveResult saveFromJson(const String& json);

}  // namespace Config
