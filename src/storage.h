#pragma once
//
// Storage: the ONLY gateway to the SD card (SD_MMC, 1-bit mode) and to the
// internal flash filesystem (LittleFS).
//
// Thread safety: every function takes one internal mutex for the whole
// operation, so any task may call any function at any time. Rules that keep it
// deadlock-free:
//   * functions here never call back into Config or into anything that takes a
//     lock of its own (Logger::log only touches RAM, so logging is fine);
//   * callers must not hold their own lock while waiting for Storage if some
//     other task could hold Storage and wait for that lock.
//
#include <Arduino.h>
#include <vector>

namespace Storage {

struct FileInfo {
  String name;   // base name, no directory
  size_t size;
};

// Call once from setup(), before any other task exists.
// Mounts LittleFS (formats it if it is unusable) and the SD card
// (3 attempts, 1 s apart, NEVER formats the card). Returns true if SD is up.
bool begin();
bool begun();      // begin() has finished: the card state below is meaningful
bool sdMounted();

// Ok      - mounted and the last write succeeded
// Missing - not mounted (no card, bad card, failed init)
// Fault   - mounted but the last write failed (read-only, corrupt FS, loose contact)
// A fault is noticed on the next write, so it can lag while nothing is written.
enum class SdState { Ok, Missing, Fault };
SdState sdState();
// Unmounts (if mounted) and tries to mount the card once. Use after repeated write
// errors or when the card was missing at boot. Safe from any task; may take ~1 s.
bool sdRemount();

// ---- SD card -------------------------------------------------------------
bool   sdReadAll(const char* path, String& out, size_t maxBytes = 16384);
// Last tailBytes of a file (capped at 32 KB); drops the leading partial line.
bool   sdReadTail(const char* path, size_t tailBytes, String& out);
// Stateless chunk read for streaming downloads. Returns bytes read, 0 at EOF.
size_t sdReadAt(const char* path, size_t offset, uint8_t* buf, size_t len);
long   sdSize(const char* path);                       // -1 if missing
bool   sdAppend(const char* path, const uint8_t* data, size_t len);
// tmpPath is written and verified first, then replaces path (remove + rename).
bool   sdWriteAtomic(const char* path, const char* tmpPath, const String& data);
// The same, but only if `path` does not exist: an existing file is never removed or overwritten (false then).
bool   sdCreate(const char* path, const char* tmpPath, const String& data);
// Replace path with an already complete tmpPath (remove + rename).
bool   sdReplace(const char* tmpPath, const char* path);
bool   sdRemove(const char* path);
bool   sdMkdir(const char* path);                      // ok if it already exists
bool   sdList(const char* dir, std::vector<FileInfo>& out);

// ---- Internal flash (LittleFS) -------------------------------------------
bool lfsRead(const char* path, String& out, size_t maxBytes = 16384);
// Skips the write when the content is identical (saves flash wear).
bool lfsWrite(const char* path, const String& data);

}  // namespace Storage
