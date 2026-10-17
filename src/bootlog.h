#pragma once
//
// BootLog: a short history of the last boots, kept in NVS (power-cut safe).
//
// Every boot adds an entry and advances its stage as startup progresses. After a
// reset loop (brown-out, crash) or a hang the next healthy boot can show what
// happened, even though nothing reached the SD log.
//
//   reason: P power-on, E external reset pin, S software, X panic, I/T/W watchdogs,
//           B brown-out, D deep sleep, O sdio, ? other
//   stage : 0 started, 1 storage up, 2 config loaded, 3 Wi-Fi started,
//           4 got an IP, 5 alive for 60 s (a healthy boot)
//
// Time. There is no battery clock, so a boot's time is recorded with its quality:
//   synced    - exact (NTP, or the clock survived a software reset); UTC epoch
//   estimated - NTP was not available: epoch is the last moment the device was
//               known to be alive, the boot happened at or after it
//   unknown   - the device never had the time
// `upMin` is how long the boot ran, to within heartbeat() granularity (minutes).
//
#include <Arduino.h>
#include <esp_system.h>

namespace BootLog {

constexpr size_t kMaxEntries = 16;  // boots kept in NVS

enum class Quality : uint8_t { Unknown = 0, Estimated = 1, Synced = 2 };

struct Entry {
  char reason;
  char stage;
  Quality quality;
  uint8_t reserved;
  uint32_t epoch;   // UTC seconds, 0 = unknown
  uint16_t upMin;   // minutes the boot ran (>= ; 0 = less than a minute)
  uint16_t reserved2;
};

// First thing in setup(): records this boot (stage 0).
void begin(esp_reset_reason_t reason);

// Marks progress of the current boot. Thread safe; cheap when nothing changes.
void stage(char stage);

// Call once the system clock becomes valid: pins this boot's exact time.
void timeSynced();

// Call every ~10 minutes: remembers "alive now" and how long this boot has run.
void heartbeat();

// Copies the entries, oldest first, the current boot last. Returns the count.
size_t entries(Entry* out, size_t max);

// "B1 B1 P4 E5 S0" (oldest first), for the log.
String summary();

const char* reasonName(char code);
const char* stageName(char code);
const char* qualityName(Quality q);

}  // namespace BootLog
