#include "ota.h"

#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "logger.h"

// The Arduino core marks a pending image valid in initArduino() unless this says "later": we confirm it ourselves once
// Wi-Fi has been up for a while (see ota.h).
extern "C" bool verifyRollbackLater() { return true; }

namespace Ota {
namespace {

const char* TAG = "ota";

SemaphoreHandle_t gMux = nullptr;
const void* gOwner = nullptr;  // the request whose upload runs, nullptr = none
size_t gTotal = 0, gReceived = 0;
uint32_t gStartMs = 0, gLastWriteMs = 0;
char gImageSha[65] = "";       // of the running image, read once at boot
bool gPending = false;         // the running image waits for its confirmation
uint32_t gWifiSinceMs = 0;     // Wi-Fi up without a break since (0 = down)

struct Lock {
  Lock() {
    if (!gMux) gMux = xSemaphoreCreateMutex();
    xSemaphoreTake(gMux, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(gMux); }
};

const char* stateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

// The state of the running image; "undefined" after flashing over the serial port (no OTA data yet).
const char* runningState() {
  esp_ota_img_states_t s;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) != ESP_OK) return "undefined";
  return stateName(s);
}

void dropLocked(const char* why) {
  if (!gOwner) return;
  Update.abort();
  LOGW(TAG, "firmware update dropped after %u of %u bytes: %s", static_cast<unsigned>(gReceived), static_cast<unsigned>(gTotal), why);
  gOwner = nullptr;
}

void confirmLocked(const char* why) {
  if (!gPending) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    gPending = false;
    LOGI(TAG, "this firmware is confirmed (%s): no rollback any more", why);
  } else {
    LOGE(TAG, "could not mark this firmware valid");
  }
}

}  // namespace

int begin(const void* owner, size_t total, const String& md5, String& err) {
  Lock lock;
  if (gOwner) {
    err = "another firmware update is running";
    return 409;
  }
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) {
    err = "no OTA slot in the partition table";
    return 400;
  }
  if (total == 0 || total > next->size) {
    err = "the firmware must be 1.." + String(static_cast<unsigned long>(next->size)) + " bytes (the size of an app slot), got " +
          String(static_cast<unsigned long>(total));
    return 413;
  }
  if (!md5.isEmpty()) {
    bool hex = md5.length() == 32;
    for (size_t i = 0; hex && i < md5.length(); ++i) hex = isxdigit(static_cast<unsigned char>(md5[i]));
    if (!hex) {
      err = "X-OTA-MD5 must be 32 hex digits";
      return 400;
    }
  }
  if (!Update.begin(total)) {
    err = String("the update could not start: ") + Update.errorString();
    Update.abort();
    return 400;
  }
  if (!md5.isEmpty()) Update.setMD5(md5.c_str());
  gOwner = owner;
  gTotal = total;
  gReceived = 0;
  gStartMs = gLastWriteMs = millis();
  LOGI(TAG, "firmware update started: %u bytes into %s%s", static_cast<unsigned>(total), next->label, md5.isEmpty() ? "" : " (with MD5)");
  return 0;
}

bool owns(const void* owner) {
  Lock lock;
  return owner && gOwner == owner;
}

bool write(const void* owner, const uint8_t* data, size_t len, String& err) {
  Lock lock;
  if (!owner || gOwner != owner) {
    err = "no firmware update runs for this request";
    return false;
  }
  if (Update.write(const_cast<uint8_t*>(data), len) != len) {
    err = String("writing the firmware failed: ") + Update.errorString();
    dropLocked(err.c_str());
    return false;
  }
  gReceived += len;
  gLastWriteMs = millis();
  return true;
}

bool finish(const void* owner, String& err, size_t& bytes, uint32_t& ms) {
  Lock lock;
  if (!owner || gOwner != owner) {
    err = "no firmware update runs for this request";
    return false;
  }
  bytes = gReceived;
  ms = millis() - gStartMs;
  if (gReceived != gTotal) {
    err = "only " + String(static_cast<unsigned>(gReceived)) + " of " + String(static_cast<unsigned>(gTotal)) + " bytes arrived";
    dropLocked(err.c_str());
    return false;
  }
  if (!Update.end()) {  // checks the image (and the MD5) before it touches the boot slot
    err = String("the firmware was refused: ") + Update.errorString();
    LOGW(TAG, "%s", err.c_str());
    Update.abort();
    gOwner = nullptr;
    return false;
  }
  gOwner = nullptr;
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  LOGI(TAG, "firmware update written: %u bytes in %lu ms, the next start runs %s (confirmed after %lu s with Wi-Fi, otherwise rolled back)",
       static_cast<unsigned>(bytes), static_cast<unsigned long>(ms), boot ? boot->label : "?", static_cast<unsigned long>(kConfirmAfterS));
  return true;
}

void abort(const void* owner, const char* why) {
  Lock lock;
  if (owner && gOwner == owner) dropLocked(why);
}

void confirm(const char* why) {
  Lock lock;
  confirmLocked(why);
}

void bootInfo() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  uint8_t sha[32];
  if (running && esp_partition_get_sha256(running, sha) == ESP_OK) {
    for (int i = 0; i < 32; ++i) snprintf(gImageSha + 2 * i, 3, "%02x", sha[i]);
  }
  const char* state = runningState();
  {
    Lock lock;
    gPending = strcmp(state, "pending_verify") == 0;
  }
  LOGI(TAG, "running %s, image %.16s..., %s", running ? running->label : "?", gImageSha,
       gPending ? "new: waits for its confirmation (Wi-Fi up for 60 s)" : state);
  if (const esp_partition_t* bad = esp_ota_get_last_invalid_partition()) {
    LOGW(TAG, "the firmware in %s did not confirm itself and was rolled back (this one runs instead)", bad->label);
  }
}

void loop() {
  Lock lock;
  if (gOwner && millis() - gLastWriteMs > kStallMs) dropLocked("nothing arrived for 30 s");
  if (!gPending) return;
  if (WiFi.status() != WL_CONNECTED) {
    gWifiSinceMs = 0;
    return;
  }
  if (!gWifiSinceMs) gWifiSinceMs = millis() ? millis() : 1;
  if (millis() - gWifiSinceMs >= kConfirmAfterS * 1000UL) confirmLocked("Wi-Fi has been up for 60 s");
}

void status(JsonObject out) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* bad = esp_ota_get_last_invalid_partition();
  Lock lock;
  out["partition"] = running ? running->label : "";
  out["state"] = runningState();
  out["image_sha256"] = gImageSha;
  out["slot_bytes"] = next ? next->size : 0;
  out["next_partition"] = next ? next->label : "";
  if (gPending) {
    if (gWifiSinceMs) {
      const uint32_t up = (millis() - gWifiSinceMs) / 1000;
      out["confirm_in_s"] = up >= kConfirmAfterS ? 0 : kConfirmAfterS - up;
    } else {
      out["confirm_in_s"] = nullptr;  // waits for Wi-Fi
    }
  }
  if (bad) {
    out["rolled_back_from"] = bad->label;
  } else {
    out["rolled_back_from"] = nullptr;
  }
  JsonObject u = out["update"].to<JsonObject>();
  u["running"] = gOwner != nullptr;
  u["received"] = gReceived;
  u["total"] = gTotal;
}

}  // namespace Ota
