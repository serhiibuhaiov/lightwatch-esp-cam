#include "logger.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "psram_alloc.h"
#include "storage.h"

namespace Logger {
namespace {

constexpr size_t kLineMax = 384;  // one line: timestamp + tag + message (device payloads are ~300 chars)
constexpr uint32_t kMaxIndex = 9999;
constexpr const char* kDir = "/logs";

constexpr size_t kFlushExtra = 128;  // room for the "lines dropped" line after a full buffer

// ---- RAM buffer: guarded by gMux ------------------------------------------
// gBuf (gBufSize bytes) and gFlushBuf (gBufSize + kFlushExtra) are allocated once by begin() and never freed.
char* gBuf = nullptr;
size_t gBufSize = 0;
size_t gWakeAt = 0;  // 3/4 full: wake the flush task
size_t gLen = 0;
uint32_t gDropped = 0;
// Lines lost because gMux could not be taken in time (counted without it, added to gDropped at the next flush).
std::atomic<uint32_t> gBusyDropped{0};
// True while the card cannot take the log (missing or faulty): the buffer then
// keeps the NEWEST lines (oldest are evicted) so the API still shows what happened.
volatile bool gRing = false;
SemaphoreHandle_t gMux = nullptr;  // also keeps Serial lines from interleaving

// ---- Flush state: guarded by gFlushMux ------------------------------------
SemaphoreHandle_t gFlushMux = nullptr;
char* gFlushBuf = nullptr;
bool gInit = false;       // card scanned, current file chosen
uint32_t gIndex = 1;      // current file number
size_t gCurSize = 0;      // bytes in the current file
uint8_t gFailStreak = 0;  // consecutive failed appends
uint32_t gLastRemountMs = 0;
constexpr uint8_t kRemountAfterFails = 2;
constexpr uint32_t kRemountEveryMs = 30000;

// Mirror of gIndex for readers that must not wait for the flush mutex (the web
// handlers run in the async_tcp task). 0 = card not scanned yet.
volatile uint32_t gPublishedIndex = 0;

// ---- Written once before the task starts, read-only afterwards -------------
Settings gSet;
TaskHandle_t gTask = nullptr;
volatile uint8_t gMinLevel = static_cast<uint8_t>(Level::Info);

char levelChar(Level l) {
  switch (l) {
    case Level::Debug: return 'D';
    case Level::Info: return 'I';
    case Level::Warn: return 'W';
    default: return 'E';
  }
}

bool timeValid() { return time(nullptr) > 1700000000; }

void formatTimestamp(char* out, size_t n) {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
  } else {
    unsigned long long ms = static_cast<unsigned long long>(esp_timer_get_time() / 1000);
    snprintf(out, n, "+%llu.%03llus", ms / 1000, ms % 1000);
  }
}

String nameForIndex(uint32_t idx) {
  char buf[16];
  snprintf(buf, sizeof(buf), "log-%04u.txt", static_cast<unsigned>(idx));
  return String(buf);
}

String pathForIndex(uint32_t idx) { return String(kDir) + "/" + nameForIndex(idx); }

bool parseIndex(const String& name, uint32_t& idx) {
  if (!isValidLogName(name)) return false;
  idx = static_cast<uint32_t>(name.substring(4, 8).toInt());
  return true;
}

// ---- Everything below runs with gFlushMux held ------------------------------

bool initFromCard() {
  Storage::sdMkdir(kDir);
  std::vector<Storage::FileInfo> files;
  if (!Storage::sdList(kDir, files)) return false;

  uint32_t maxIdx = 0;
  for (const auto& f : files) {
    uint32_t idx;
    if (parseIndex(f.name, idx) && idx > maxIdx) maxIdx = idx;
  }
  gIndex = maxIdx ? maxIdx : 1;
  long size = Storage::sdSize(pathForIndex(gIndex).c_str());
  gCurSize = size > 0 ? static_cast<size_t>(size) : 0;
  gInit = true;
  gPublishedIndex = gIndex;
  return true;
}

// Makes sure the card is mounted; re-tries a missing card every kRemountEveryMs.
bool ensureCard() {
  if (Storage::sdMounted()) return true;
  uint32_t now = millis();
  if (gLastRemountMs && now - gLastRemountMs < kRemountEveryMs) return false;
  gLastRemountMs = now ? now : 1;
  if (!Storage::sdRemount()) return false;
  gInit = false;  // rescan: the card may be a different one
  gPublishedIndex = 0;
  gFailStreak = 0;
  return true;
}

void listIndexes(std::vector<uint32_t>& out) {
  out.clear();
  std::vector<Storage::FileInfo> files;
  if (!Storage::sdList(kDir, files)) return;
  for (const auto& f : files) {
    uint32_t idx;
    if (parseIndex(f.name, idx)) out.push_back(idx);
  }
  std::sort(out.begin(), out.end());
}

// Called when a new file is about to be started: keep keepFiles in total.
void prune() {
  std::vector<uint32_t> idx;
  listIndexes(idx);
  size_t keepOld = gSet.keepFiles > 0 ? gSet.keepFiles - 1 : 0;
  while (idx.size() > keepOld) {
    Storage::sdRemove(pathForIndex(idx.front()).c_str());
    idx.erase(idx.begin());
  }
}

void rotateIfNeeded(size_t incoming) {
  size_t maxBytes = static_cast<size_t>(gSet.maxFileKb) * 1024;
  if (gCurSize == 0 || gCurSize + incoming <= maxBytes) return;

  if (gIndex >= kMaxIndex) {  // file names have 4 digits: start over
    std::vector<uint32_t> idx;
    listIndexes(idx);
    for (uint32_t i : idx) Storage::sdRemove(pathForIndex(i).c_str());
    gIndex = 0;
  }
  ++gIndex;
  gCurSize = 0;
  gPublishedIndex = gIndex;
  prune();
}

void requeue(size_t n) {
  if (xSemaphoreTake(gMux, portMAX_DELAY) != pdTRUE) return;
  if (gLen + n <= gBufSize) {
    memmove(gBuf + n, gBuf, gLen);
    memcpy(gBuf, gFlushBuf, n);
    gLen += n;
  } else {
    for (size_t i = 0; i < n; ++i) {  // count whole lines, not one per failed batch
      if (gFlushBuf[i] == '\n') gDropped++;
    }
  }
  xSemaphoreGive(gMux);
}

bool flushLocked(bool force) {
  if (!gBuf) return false;
  if (!force && millis() < static_cast<uint32_t>(gSet.quietSec) * 1000UL) return true;
  if (!ensureCard()) return false;
  if (!gInit && !initFromCard()) return false;

  size_t n = 0;
  uint32_t dropped = 0;
  if (xSemaphoreTake(gMux, portMAX_DELAY) == pdTRUE) {
    n = gLen;
    dropped = gDropped + gBusyDropped.exchange(0);
    memcpy(gFlushBuf, gBuf, n);
    gLen = 0;
    gDropped = 0;
    xSemaphoreGive(gMux);
  }

  if (dropped) {
    char ts[32];
    formatTimestamp(ts, sizeof(ts));
    const size_t room = gBufSize + kFlushExtra - n;
    int m = snprintf(gFlushBuf + n, room,
                     "%s W logger: %lu lines dropped (buffer full or busy)\n", ts,
                     static_cast<unsigned long>(dropped));
    if (m > 0 && static_cast<size_t>(m) < room) n += m;
  }
  if (n == 0) return true;

  rotateIfNeeded(n);
  if (Storage::sdAppend(pathForIndex(gIndex).c_str(),
                        reinterpret_cast<const uint8_t*>(gFlushBuf), n)) {
    gCurSize += n;
    gFailStreak = 0;
    return true;
  }

  // Write failed: put the lines back in front of the RAM buffer and retry later.
  requeue(n);
  if (gFailStreak == 0) {  // one warning per outage, not one per flush
    log(Level::Warn, "logger", "SD append failed, %u bytes kept in RAM", static_cast<unsigned>(n));
  }

  // A supply glitch can leave the card unusable while the MCU keeps running:
  // after repeated failures drop the mount and let ensureCard() bring it back.
  if (++gFailStreak >= kRemountAfterFails) {
    gLastRemountMs = millis() ? millis() : 1;
    if (Storage::sdRemount()) {
      gInit = false;
      gPublishedIndex = 0;
      gFailStreak = 0;
    }
  }
  return false;
}

// ---- Flush task -------------------------------------------------------------

void anchorTime() {
  static bool anchored = false;  // only this task touches it
  if (anchored || !timeValid()) return;
  anchored = true;
  log(Level::Info, "time", "clock synced; uptime was %llu ms at this moment",
      static_cast<unsigned long long>(esp_timer_get_time() / 1000));
}

void flushTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(static_cast<uint32_t>(gSet.flushSec) * 1000UL));
    anchorTime();
    xSemaphoreTake(gFlushMux, portMAX_DELAY);
    flushLocked(false);
    xSemaphoreGive(gFlushMux);
    gRing = Storage::sdState() != Storage::SdState::Ok;
  }
}

// Both buffers of `size` bytes from the memory `caps` describe; false (nothing kept) if either does not fit.
bool allocBuffers(size_t size, uint32_t caps) {
  char* buf = static_cast<char*>(heap_caps_malloc(size, caps));
  char* flush = static_cast<char*>(heap_caps_malloc(size + kFlushExtra, caps));
  if (!buf || !flush) {
    free(buf);
    free(flush);
    return false;
  }
  gBuf = buf;
  gFlushBuf = flush;
  gBufSize = size;
  gWakeAt = size * 3 / 4;
  return true;
}

}  // namespace

// ---- Public API ---------------------------------------------------------------

void begin() {
  // PSRAM when the board has it; else (or if it could not give them) 8 KB of the internal RAM; failing that, Serial only.
  if (!gBuf && !(psramAvailable() && allocBuffers(kRamBufferBytesPsram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
    allocBuffers(kRamBufferBytesInternal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!gMux) gMux = xSemaphoreCreateMutex();
  if (!gFlushMux) gFlushMux = xSemaphoreCreateMutex();
}

size_t ramBufferBytes() { return gBufSize; }

void start(const Settings& settings) {
  if (gTask) return;
  gSet = settings;
  gMinLevel = static_cast<uint8_t>(settings.minLevel);
  gRing = Storage::sdState() != Storage::SdState::Ok;
  begin();
  xTaskCreate(flushTask, "logflush", 8192, nullptr, 1, &gTask);
}

void log(Level level, const char* tag, const char* fmt, ...) {
  if (static_cast<uint8_t>(level) < gMinLevel) return;

  char msg[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  for (char* p = msg; *p; ++p) {
    if (*p == '\n' || *p == '\r') *p = ' ';
  }

  char ts[32];
  formatTimestamp(ts, sizeof(ts));

  char line[kLineMax];
  int n = snprintf(line, sizeof(line) - 1, "%s %c %s: %s", ts, levelChar(level), tag, msg);
  if (n < 0) return;
  if (static_cast<size_t>(n) > sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n';
  line[n] = '\0';

  if (!gMux || !gBuf) {  // before begin() (or no memory for the buffer): Serial only
    Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    return;
  }
  if (xSemaphoreTake(gMux, pdMS_TO_TICKS(100)) != pdTRUE) {
    gBusyDropped.fetch_add(1);
    return;
  }

  Serial.write(reinterpret_cast<const uint8_t*>(line), n);

  bool wake = false;
  if (gLen + n > gBufSize && gRing) {
    // Evict whole oldest lines until the new one fits.
    size_t need = gLen + n - gBufSize;
    const char* nl = static_cast<const char*>(memchr(gBuf + need - 1, '\n', gLen - need + 1));
    size_t cut = nl ? static_cast<size_t>(nl - gBuf) + 1 : gLen;
    memmove(gBuf, gBuf + cut, gLen - cut);
    gLen -= cut;
  }
  if (gLen + n <= gBufSize) {
    size_t before = gLen;
    memcpy(gBuf + gLen, line, n);
    gLen += n;
    wake = before < gWakeAt && gLen >= gWakeAt;
  } else {
    gDropped++;
  }
  xSemaphoreGive(gMux);

  if (wake && gTask) xTaskNotifyGive(gTask);
}

void requestFlush() {
  if (gTask) xTaskNotifyGive(gTask);
}

bool flushBlocking() {
  if (!gFlushMux) return false;
  xSemaphoreTake(gFlushMux, portMAX_DELAY);
  bool ok = flushLocked(true);
  xSemaphoreGive(gFlushMux);
  return ok;
}

// Called from the web task: never waits for a flush in progress. While the card
// has not been scanned yet it tries the flush mutex briefly, then gives up with "".
String currentFileName() {
  if (!gFlushMux) return "";
  uint32_t idx = gPublishedIndex;
  if (idx) return nameForIndex(idx);
  if (!Storage::sdMounted() || xSemaphoreTake(gFlushMux, pdMS_TO_TICKS(200)) != pdTRUE) return "";
  if (!gInit) initFromCard();
  String name = gInit ? nameForIndex(gIndex) : String("");
  xSemaphoreGive(gFlushMux);
  return name;
}

void copyPending(String& out) {
  out = "";
  if (!gMux || xSemaphoreTake(gMux, pdMS_TO_TICKS(100)) != pdTRUE) return;
  out.concat(gBuf, static_cast<unsigned int>(gLen));
  xSemaphoreGive(gMux);
}

bool isValidLogName(const String& name) {
  if (name.length() != 12 || !name.startsWith("log-") || !name.endsWith(".txt")) return false;
  for (int i = 4; i < 8; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

String pathFor(const String& name) { return String(kDir) + "/" + name; }

const char* levelName(Level level) {
  switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    default: return "error";
  }
}

Level levelFromString(const String& text, Level fallback) {
  if (text.equalsIgnoreCase("debug")) return Level::Debug;
  if (text.equalsIgnoreCase("info")) return Level::Info;
  if (text.equalsIgnoreCase("warn")) return Level::Warn;
  if (text.equalsIgnoreCase("error")) return Level::Error;
  return fallback;
}

}  // namespace Logger
