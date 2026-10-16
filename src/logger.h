#pragma once
//
// Logger: thread-safe, power-cut-friendly logging to Serial and to the SD card.
//
//   * Any task may call Logger::log()/LOGx() concurrently. Not for ISRs.
//   * log() only formats a line and copies it into an 8 KB RAM buffer under a
//     short mutex. It never touches the SD card, so it is fast and cannot block
//     on storage.
//   * One background task ("logflush") writes the buffer to the card in batches
//     (open, append, close) every flushSec, or when the buffer is 3/4 full.
//   * For the first quietSec after boot nothing is written to the card (power
//     often flickers right after it returns); lines wait in RAM.
//   * If the card disappears or keeps failing, the flush task remounts it (about
//     every 30 s) and keeps the unwritten lines in RAM meanwhile.
//   * Files are /logs/log-NNNN.txt, rotated by size, oldest pruned (keepFiles).
//     No renames, so a power cut can damage at most the file being appended to.
//   * Timestamps are "+uptime" until NTP syncs, then local date and time.
//     When time first becomes valid one "synced" line ties the two together.
//
// Lock order (never violate): flush mutex -> buffer mutex (short) ... and the
// Storage mutex is taken only with the buffer mutex released.
//
#include <Arduino.h>

namespace Logger {

enum class Level : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct Settings {
  uint16_t flushSec = 20;
  uint16_t maxFileKb = 1024;
  uint8_t keepFiles = 5;
  uint16_t quietSec = 30;
  Level minLevel = Level::Info;
};

// Creates the mutexes. Call first thing in setup(); log() works (Serial only)
// even before this.
void begin();

// Applies settings and starts the flush task. Call once, after Config::load().
void start(const Settings& settings);

void log(Level level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Ask the flush task to write now (respects the boot quiet period). Non-blocking.
void requestFlush();

// Write everything to the card right now, ignoring the quiet period. Blocks
// until done. Use before a software restart. Returns false if the SD is not
// available or the write failed.
bool flushBlocking();

// Name of the file currently being appended to ("" until the card is usable).
String currentFileName();

// Lines that are still in the RAM buffer and not on the card yet (the boot quiet
// period plus up to flushSec). Empty if the buffer mutex is busy.
void copyPending(String& out);

// "log-NNNN.txt" with exactly 4 digits. Use to validate names from the network.
bool isValidLogName(const String& name);
String pathFor(const String& name);  // "/logs/<name>"

const char* levelName(Level level);
Level levelFromString(const String& text, Level fallback);

}  // namespace Logger

#define LOGD(tag, ...) Logger::log(Logger::Level::Debug, tag, __VA_ARGS__)
#define LOGI(tag, ...) Logger::log(Logger::Level::Info, tag, __VA_ARGS__)
#define LOGW(tag, ...) Logger::log(Logger::Level::Warn, tag, __VA_ARGS__)
#define LOGE(tag, ...) Logger::log(Logger::Level::Error, tag, __VA_ARGS__)
