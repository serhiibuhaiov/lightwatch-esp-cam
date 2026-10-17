#include "bootlog.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace BootLog {
namespace {

constexpr const char* kNs = "boot";
constexpr const char* kKeyEntries = "hb";   // blob of Entry[]
constexpr const char* kKeySeen = "seen";    // last epoch the device was known to be alive
constexpr time_t kMinValidTime = 1700000000;

SemaphoreHandle_t gMux = nullptr;
Entry gE[kMaxEntries];
size_t gN = 0;
uint32_t gSeen = 0;

bool timeValid() { return time(nullptr) > kMinValidTime; }

char reasonCode(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return 'P';
    case ESP_RST_EXT: return 'E';
    case ESP_RST_SW: return 'S';
    case ESP_RST_PANIC: return 'X';
    case ESP_RST_INT_WDT: return 'I';
    case ESP_RST_TASK_WDT: return 'T';
    case ESP_RST_WDT: return 'W';
    case ESP_RST_DEEPSLEEP: return 'D';
    case ESP_RST_BROWNOUT: return 'B';
    case ESP_RST_SDIO: return 'O';
    default: return '?';
  }
}

void save() {
  Preferences p;
  if (p.begin(kNs, false)) {
    p.putBytes(kKeyEntries, gE, gN * sizeof(Entry));
    p.putUInt(kKeySeen, gSeen);
    p.end();
  }
}

struct Lock {
  Lock() { xSemaphoreTake(gMux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(gMux); }
};

}  // namespace

void begin(esp_reset_reason_t reason) {
  if (!gMux) gMux = xSemaphoreCreateMutex();
  Lock lock;
  Preferences p;
  if (p.begin(kNs, true)) {
    size_t len = p.getBytesLength(kKeyEntries);
    if (len && len % sizeof(Entry) == 0 && len / sizeof(Entry) <= kMaxEntries) {
      p.getBytes(kKeyEntries, gE, len);
      gN = len / sizeof(Entry);
    }
    gSeen = p.getUInt(kKeySeen, 0);
    p.end();
  }

  Entry e = {};
  e.reason = reasonCode(reason);
  e.stage = '0';
  if (timeValid()) {  // warm restart: the RTC kept the clock
    e.epoch = static_cast<uint32_t>(time(nullptr));
    e.quality = Quality::Synced;
  } else if (gSeen) {
    e.epoch = gSeen;  // no NTP yet: not earlier than the last moment we were alive
    e.quality = Quality::Estimated;
  }
  if (gN == kMaxEntries) {
    for (size_t i = 1; i < gN; ++i) gE[i - 1] = gE[i];
    --gN;
  }
  gE[gN++] = e;
  save();
}

void stage(char s) {
  if (!gMux || !gN) return;
  Lock lock;
  Entry& cur = gE[gN - 1];
  bool changed = false;
  if (s > cur.stage) {  // stages only move forward
    cur.stage = s;
    changed = true;
  }
  if (s == '5' && cur.upMin < 1) {
    cur.upMin = 1;
    changed = true;
  }
  if (changed) save();
}

void timeSynced() {
  if (!gMux || !gN || !timeValid()) return;
  Lock lock;
  Entry& cur = gE[gN - 1];
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  uint32_t boot = now - static_cast<uint32_t>(millis() / 1000);
  if (cur.quality == Quality::Synced && cur.epoch == boot) return;
  cur.epoch = boot;
  cur.quality = Quality::Synced;
  gSeen = now;
  save();
}

void heartbeat() {
  if (!gMux || !gN) return;
  Lock lock;
  Entry& cur = gE[gN - 1];
  uint32_t min = millis() / 60000UL;
  cur.upMin = min > 65535 ? 65535 : static_cast<uint16_t>(min);
  if (timeValid()) gSeen = static_cast<uint32_t>(time(nullptr));
  save();
}

size_t entries(Entry* out, size_t max) {
  if (!gMux) return 0;
  Lock lock;
  size_t n = gN < max ? gN : max;
  for (size_t i = 0; i < n; ++i) out[i] = gE[gN - n + i];
  return n;
}

String summary() {
  if (!gMux) return "";
  Lock lock;
  String out;
  for (size_t i = 0; i < gN; ++i) {
    if (i) out += ' ';
    out += gE[i].reason;
    out += gE[i].stage;
  }
  return out;
}

const char* reasonName(char c) {
  switch (c) {
    case 'P': return "POWERON";
    case 'E': return "EXT";
    case 'S': return "SW";
    case 'X': return "PANIC";
    case 'I': return "INT_WDT";
    case 'T': return "TASK_WDT";
    case 'W': return "WDT";
    case 'D': return "DEEPSLEEP";
    case 'B': return "BROWNOUT";
    case 'O': return "SDIO";
    default: return "UNKNOWN";
  }
}

const char* stageName(char c) {
  switch (c) {
    case '0': return "started";
    case '1': return "storage";
    case '2': return "config";
    case '3': return "wifi-started";
    case '4': return "got-ip";
    case '5': return "alive-60s";
    default: return "?";
  }
}

const char* qualityName(Quality q) {
  switch (q) {
    case Quality::Synced: return "synced";
    case Quality::Estimated: return "estimated";
    default: return "unknown";
  }
}

}  // namespace BootLog
