#include "indicator.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "storage.h"

namespace Indicator {
namespace {

constexpr int kFlashPin = 4;          // flash LED; also SD DAT1, unused in 1-bit mode
constexpr int kRedPin = 33;           // red LED, active LOW
constexpr uint8_t kFlashDuty = 32;    // of 255: visible, but easy on the supply
constexpr uint32_t kPwmHz = 4000;
constexpr uint32_t kWifiGraceMs = 30000;  // a normal connect takes a few seconds

volatile bool gReady = false;
enum class Wifi : uint8_t { NotUsed, Down, Up };
volatile Wifi gWifi = Wifi::NotUsed;
volatile uint32_t gDownSince = 0;  // when Wi-Fi last went down (or boot, if it never came up)

void flashPulse(uint32_t onMs) {
  // Attach PWM for the pulse only and give the pin back afterwards: Storage resets
  // GPIO4 with pinMode() on every (re)mount, which would silently detach it.
  ledcAttach(kFlashPin, kPwmHz, 8);
  ledcWrite(kFlashPin, kFlashDuty);
  vTaskDelay(pdMS_TO_TICKS(onMs));
  ledcDetach(kFlashPin);
  pinMode(kFlashPin, OUTPUT);
  digitalWrite(kFlashPin, LOW);
}

void shortPulses(int n) {
  for (int i = 0; i < n; ++i) {
    flashPulse(80);
    vTaskDelay(pdMS_TO_TICKS(160));
  }
}

// Red LED: own task, so the error patterns never stall the startup blinking.
void redTask(void*) {
  bool on = false;
  for (;;) {
    if (gReady) {
      digitalWrite(kRedPin, LOW);  // steady on
      vTaskDelay(pdMS_TO_TICKS(200));
    } else {
      on = !on;
      digitalWrite(kRedPin, on ? LOW : HIGH);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

void errorTask(void*) {
  for (;;) {
    bool any = false;
    // Before Storage::begin() the card state is unknown, not "missing".
    if (Storage::begun()) {
      switch (Storage::sdState()) {
        case Storage::SdState::Missing:
          shortPulses(3);
          vTaskDelay(pdMS_TO_TICKS(1200));
          any = true;
          break;
        case Storage::SdState::Fault:
          flashPulse(600);
          vTaskDelay(pdMS_TO_TICKS(1200));
          any = true;
          break;
        default:
          break;
      }
    }
    if (gWifi == Wifi::Down && millis() - gDownSince > kWifiGraceMs) {
      shortPulses(2);
      vTaskDelay(pdMS_TO_TICKS(1200));
      any = true;
    }
    vTaskDelay(pdMS_TO_TICKS(any ? 1300 : 500));
  }
}

}  // namespace

void begin() {
  pinMode(kRedPin, OUTPUT);
  digitalWrite(kRedPin, HIGH);  // off until the red task takes over
  xTaskCreate(redTask, "led_red", 2048, nullptr, 1, nullptr);
  xTaskCreate(errorTask, "led_err", 3072, nullptr, 1, nullptr);
}

void ready() { gReady = true; }

void wifiConfigured(bool configured) {
  if (!configured) {
    gWifi = Wifi::NotUsed;
  } else if (gWifi == Wifi::NotUsed) {
    gDownSince = millis();
    gWifi = Wifi::Down;
  }
}

void wifiUp(bool up) {
  if (gWifi == Wifi::NotUsed) return;
  if (!up && gWifi != Wifi::Down) gDownSince = millis();  // a new outage; repeated events do not restart the clock
  gWifi = up ? Wifi::Up : Wifi::Down;
}

}  // namespace Indicator
