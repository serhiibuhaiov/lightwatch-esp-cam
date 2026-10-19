#include "storage.h"

#include <FS.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

#include <errno.h>
#include <string.h>

#include "logger.h"

namespace Storage {
namespace {

const char* TAG = "storage";
constexpr size_t kMaxTail = kMaxTailBytes;

SemaphoreHandle_t gMutex = nullptr;
volatile bool gSdMounted = false;
volatile bool gBegun = false;
volatile bool gWriteFault = false;  // last SD write failed

struct Guard {
  Guard() {
    if (!gMutex) gMutex = xSemaphoreCreateMutex();  // normally created in begin()
    xSemaphoreTake(gMutex, portMAX_DELAY);
  }
  ~Guard() { xSemaphoreGive(gMutex); }
};

bool readFrom(fs::FS& fs, const char* path, String& out, size_t maxBytes) {
  File f = fs.open(path, FILE_READ);
  if (!f || f.isDirectory()) return false;
  size_t size = f.size();
  if (size > maxBytes) {
    f.close();
    return false;
  }
  out = "";
  out.reserve(size);
  uint8_t buf[256];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.concat(reinterpret_cast<const char*>(buf), static_cast<unsigned int>(n));
  }
  f.close();
  return true;
}

// One error line per failed step of an SD write, so a random failure says where and why. `err` is errno saved
// right after the failing call; `got`/`want` < 0 means no such size. An SD driver that found no DMA buffer shows
// up as EIO, not ENOMEM, hence the largest free 8-bit DRAM block.
void logSdFailure(const char* target, const char* step, const char* path, int err, long got, long want) {
  char sizes[40] = "";
  if (got >= 0 && want >= 0) {
    snprintf(sizes, sizeof(sizes), ", %ld of %ld B", got, want);
  } else if (want >= 0) {
    snprintf(sizes, sizeof(sizes), ", %ld B", want);
  }
  const unsigned largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  LOGE(TAG, "write %s: %s %s failed%s, errno %d (%s), dram largest %u B", target, step, path, sizes, err,
       strerror(err), largest);
}

}  // namespace

// Called with the mutex held. 1-bit mode: GPIO 14 (CLK), 15 (CMD), 2 (DAT0).
// Never formats the card.
static bool mountSdLocked(int attempts, uint32_t retryDelayMs, bool quiet) {
  gSdMounted = false;
  for (int attempt = 1; attempt <= attempts && !gSdMounted; ++attempt) {
    if (SD_MMC.begin("/sdcard", true, false) && SD_MMC.cardType() != CARD_NONE) {
      gSdMounted = true;
      LOGI(TAG, "SD mounted, %llu MB", SD_MMC.cardSize() / (1024ULL * 1024ULL));
    } else {
      // Periodic re-tries of a missing card must not flood the (RAM only) log.
      if (quiet) {
        LOGD(TAG, "SD mount attempt %d failed", attempt);
      } else {
        LOGW(TAG, "SD mount attempt %d failed", attempt);
      }
      SD_MMC.end();
      if (attempt < attempts) delay(retryDelayMs);
    }
  }

  // GPIO 4 is the flash LED and also SD DAT1 in 4-bit mode. We use 1-bit mode,
  // so take the pin back and keep the LED off.
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);

  return gSdMounted;
}

bool begin() {
  Guard g;

  if (LittleFS.begin(true)) {
    LOGI(TAG, "LittleFS mounted");
  } else {
    LOGE(TAG, "LittleFS mount failed");
  }

  bool ok = mountSdLocked(3, 1000, false);
  gBegun = true;
  return ok;
}

bool begun() { return gBegun; }

bool sdRemount() {
  Guard g;
  const bool wasMounted = gSdMounted;
  if (wasMounted) SD_MMC.end();  // drop the stale state before trying again
  bool ok = mountSdLocked(1, 0, true);
  if (ok) gWriteFault = false;  // judged again by the next write
  if (ok && !wasMounted) LOGW(TAG, "SD card became available");
  if (ok && wasMounted) LOGW(TAG, "SD card remounted after write errors");
  return ok;
}

bool sdMounted() { return gSdMounted; }

SdState sdState() {
  if (!gSdMounted) return SdState::Missing;
  return gWriteFault ? SdState::Fault : SdState::Ok;
}

// ---- SD card -------------------------------------------------------------

bool sdReadAll(const char* path, String& out, size_t maxBytes) {
  Guard g;
  if (!gSdMounted) return false;
  return readFrom(SD_MMC, path, out, maxBytes);
}

bool sdReadTail(const char* path, size_t tailBytes, String& out) {
  if (tailBytes > kMaxTail) tailBytes = kMaxTail;
  Guard g;
  if (!gSdMounted) return false;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) return false;

  size_t size = f.size();
  size_t start = size > tailBytes ? size - tailBytes : 0;
  if (start > 0) f.seek(start);

  out = "";
  out.reserve(size - start);
  uint8_t buf[256];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.concat(reinterpret_cast<const char*>(buf), static_cast<unsigned int>(n));
  }
  f.close();

  if (start > 0) {  // we most likely started mid-line
    int nl = out.indexOf('\n');
    if (nl >= 0) out.remove(0, nl + 1);
  }
  return true;
}

size_t sdReadAt(const char* path, size_t offset, uint8_t* buf, size_t len) {
  Guard g;
  if (!gSdMounted) return 0;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) return 0;
  if (offset >= f.size()) {
    f.close();
    return 0;
  }
  f.seek(offset);
  size_t n = f.read(buf, len);
  f.close();
  return n;
}

long sdSize(const char* path) {
  Guard g;
  if (!gSdMounted) return -1;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) return -1;
  long size = static_cast<long>(f.size());
  f.close();
  return size;
}

bool sdAppend(const char* path, const uint8_t* data, size_t len) {
  Guard g;
  if (!gSdMounted) return false;
  File f = SD_MMC.open(path, FILE_APPEND);
  if (!f) {
    gWriteFault = true;
    return false;
  }
  size_t written = f.write(data, len);
  f.close();  // closing is what commits the FAT entry; never keep files open
  gWriteFault = written != len;
  return !gWriteFault;
}

// `replace` = false: only creates `path`; an existing file (even one that a card error hid from an earlier look) is
// never removed, and the FAT rename refuses to overwrite it.
static bool writeAtomicLocked(const char* path, const char* tmpPath, const String& data, bool replace = true) {
  const long want = static_cast<long>(data.length());
  if (SD_MMC.exists(tmpPath)) SD_MMC.remove(tmpPath);
  errno = 0;
  File f = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!f) {
    logSdFailure(path, "open", tmpPath, errno, -1, want);  // the VFS stat()s after a failed fopen: errno is a hint
    return false;
  }
  errno = 0;
  size_t written = f.write(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
  f.close();
  const int writeErr = errno;  // fwrite buffers: a card error may only come from the flush in close
  if (written != data.length()) {
    logSdFailure(path, "write", tmpPath, writeErr, static_cast<long>(written), want);
    return false;
  }

  // Verify what actually landed on the card before touching the live file.
  errno = 0;
  File check = SD_MMC.open(tmpPath, FILE_READ);
  const long got = check ? static_cast<long>(check.size()) : -1;
  const int checkErr = writeErr ? writeErr : errno;
  if (check) check.close();
  if (got != want) {
    logSdFailure(path, "check", tmpPath, checkErr, got, want);
    return false;
  }

  // Once the old file is removed below, a power cut leaves a complete, valid
  // tmp file that Config::load() (and Devices/Scenario load()) promotes at the next boot. A cut before that
  // keeps the old file and the new tmp is discarded (the save was never acknowledged).
  if (SD_MMC.exists(path)) {
    if (!replace) {
      SD_MMC.remove(tmpPath);
      return false;
    }
    errno = 0;
    if (!SD_MMC.remove(path)) {
      logSdFailure(path, "remove", path, errno, -1, want);
      return false;
    }
  }
  errno = 0;
  if (!SD_MMC.rename(tmpPath, path)) {
    logSdFailure(path, "rename", tmpPath, errno, -1, want);  // EEXIST: `path` was there but exists() missed it
    return false;
  }
  return true;
}

bool sdWriteAtomic(const char* path, const char* tmpPath, const String& data) {
  Guard g;
  if (!gSdMounted) return false;
  bool ok = writeAtomicLocked(path, tmpPath, data);
  gWriteFault = !ok;
  return ok;
}

bool sdCreate(const char* path, const char* tmpPath, const String& data) {
  Guard g;
  if (!gSdMounted) return false;
  return writeAtomicLocked(path, tmpPath, data, false);  // "already there" is not a write fault
}

bool sdReplace(const char* tmpPath, const char* path) {
  Guard g;
  if (!gSdMounted) return false;
  errno = 0;
  if (SD_MMC.exists(path) && !SD_MMC.remove(path)) {
    logSdFailure(path, "remove", path, errno, -1, -1);
    return false;
  }
  errno = 0;
  if (!SD_MMC.rename(tmpPath, path)) {
    logSdFailure(path, "rename", tmpPath, errno, -1, -1);
    return false;
  }
  return true;
}

bool sdRemove(const char* path) {
  Guard g;
  if (!gSdMounted) return false;
  return SD_MMC.exists(path) && SD_MMC.remove(path);
}

bool sdMkdir(const char* path) {
  Guard g;
  if (!gSdMounted) return false;
  if (SD_MMC.exists(path)) return true;
  return SD_MMC.mkdir(path);
}

bool sdList(const char* dir, std::vector<FileInfo>& out) {
  out.clear();
  Guard g;
  if (!gSdMounted) return false;
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return false;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    if (e.isDirectory()) continue;
    String name = e.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    out.push_back({name, static_cast<size_t>(e.size())});
  }
  d.close();
  return true;
}

// ---- Internal flash ------------------------------------------------------

bool lfsRead(const char* path, String& out, size_t maxBytes) {
  Guard g;
  return readFrom(LittleFS, path, out, maxBytes);
}

bool lfsWrite(const char* path, const String& data) {
  Guard g;

  String current;
  if (readFrom(LittleFS, path, current, 16384) && current == data) return true;

  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) return false;
  size_t written = f.write(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
  f.close();
  if (written != data.length()) {
    LittleFS.remove(tmp);
    return false;
  }
  // No remove() first: LittleFS rename replaces an existing file atomically,
  // so a power cut leaves either the old or the new file, never neither.
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    return false;
  }
  return true;
}

}  // namespace Storage
