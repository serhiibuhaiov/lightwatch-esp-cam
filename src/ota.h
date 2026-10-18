#pragma once
//
// Firmware update over HTTP (POST /api/ota, see web_api.cpp). The new image is written into the other app slot
// (min_spiffs.csv: app0 and app1, 1.875 MB each) while the firmware runs; Update.end() verifies it (the SHA-256 appended
// to the image, and the MD5 when the uploader gave one) and only then makes it the boot slot, so an upload cut short or
// a broken file leaves the running firmware as it is. The restart afterwards is a software one: power_on scenarios do
// not start, running ones go on as on_interrupt says.
//
// Rollback (the bootloader has CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a new image starts "pending verify". It is marked
// valid once Wi-Fi has been up for kConfirmAfterS without a break (the web server starts in setup(), before Wi-Fi), or
// right before a restart asked for over the web (that request proved the web server works). If it crashes or restarts
// before that (a panic, a watchdog, the Wi-Fi watchdog, a power cut), the bootloader goes back to the previous firmware.
//
// One upload at a time. All functions are safe from any task (one internal mutex).
//
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Ota {

constexpr uint32_t kConfirmAfterS = 60;
constexpr uint32_t kStallMs = 30000;  // an upload that sends nothing for this long is dropped

// Starts an upload of `total` bytes for `owner` (the HTTP request). `md5`: "" or 32 hex digits, checked at the end.
// Returns 0, or the HTTP status of the refusal with `err` set: 409 another upload runs, 413 larger than the slot,
// 400 otherwise (bad MD5, Update refused).
int begin(const void* owner, size_t total, const String& md5, String& err);

// True while `owner` has the upload that runs.
bool owns(const void* owner);

// Writes the next piece. False: the upload failed (wrong first byte, flash error) and was dropped; `err` says why.
bool write(const void* owner, const uint8_t* data, size_t len, String& err);

// All bytes are in: verifies the image and makes it the boot slot. False: dropped, `err` says why.
// `bytes`/`ms`: what was written and how long the upload took.
bool finish(const void* owner, String& err, size_t& bytes, uint32_t& ms);

// Drops the upload of `owner` (no-op if it has none).
void abort(const void* owner, const char* why);

// Marks a pending image valid now (before an intentional restart). No-op otherwise.
void confirm(const char* why);

// Once at boot, after the logger has started: which slot runs, its state and image hash, an earlier rollback.
void bootInfo();

// From loop(): confirms a pending image after kConfirmAfterS of Wi-Fi; drops an upload stalled for kStallMs.
void loop();

// GET /api/status -> firmware: partition, state, image_sha256, slot_bytes, next_partition, confirm_in_s,
// rolled_back_from, update {running, received, total}.
void status(JsonObject out);

}  // namespace Ota
